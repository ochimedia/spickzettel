#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "core/canvas/canvas_manager.h"
#include "core/persistence/image_codec.h"

namespace sz::core::persistence {

// Reads and writes the on-disk library (folders/canvases/items/strokes,
// plus each Shot item's captured screenshot pixels) under one root
// directory - the sole persistence boundary for "everything" the overlay
// shows: there's no explicit save/load anywhere else in the app (see
// OverlayApp's debounced autosave, which is what actually calls Save()).
// Pure <filesystem>/<fstream> plus the vendored QOI codec - no
// OS-specific dependency, so (unlike the real screen-capture code it
// complements) this is fully exercised by linux-tests.
//
// Layout under `rootDir`:
//   library.json                     - currentFolderId and currentCanvasId,
//                                       and nothing else
//   folders/order.json               - the folders' uids, in order
//   folders/<folder>/folder.json     - that folder's id and name
//   folders/<folder>/order.json      - its canvases' uids, in order
//   folders/<folder>/<canvas>/canvas.json
//                                     - that canvas's id and name
//   folders/<folder>/<canvas>/order.json
//                                     - its snippets' uids, back to
//                                       front: snippet order is z-order
//   folders/<folder>/<canvas>/<snippet>/item.json
//                                     - the snippet's record
//   folders/<folder>/<canvas>/<snippet>/<uid>.qoi
//                                     - its captured pixels, and beside it
//                                       <uid>.thumb.qoi for the Overview
//   staging/                           - see kStagingDir
//   retired/<folder>/<canvas>/<snippet>/
//                                     - what a save found the library no
//                                       longer holding, set aside whole at
//                                       the path it had under folders/
//                                       rather than deleted; see Save
//   retired/staging/                   - pictures found in staging that no
//                                       snippet named, set aside for the
//                                       same reason; see Save's last pass
//   .../<any directory>/.removed        - the directory was deleted for good
//                                       and could not be wholly removed
//                                       yet; nothing in it is read, and
//                                       every save tries again. See Remove.
//
// Every id inside a record is spelled the way the directory names spell
// it - six base36 characters, see util/uid.h - so a record and the
// directory holding it can be matched by eye (see IdJson/ReadId).
//
// A directory per folder, per canvas and per snippet, named
// "<slug of its name>-<uid>"
// (see MakeSlug). A tree rather than one file because one file is
// rewritten whole on every save: 50 canvases of ordinary drawing is a 42 MB
// document taking half a second to serialize, on the render thread, every
// couple of seconds of quiet. What a save does is bounded by what moved -
// see the block on folderDirs_/writtenItemHashes_ below, and docs/PERF.md.
//
// **The tree is the library, and every file in it describes itself.** There
// is no index that says which canvas is in which folder - the directory it
// sits in says that, and where a record disagrees with where it physically
// is, the filesystem wins. That is the point rather than a side effect:
// rearranging the library in a file manager while the app is closed is a
// supported way to use it, so Load reconciles rather than validates:
//
//   - a directory with no folder.json/canvas.json in it is not ours, and is
//     left alone rather than deleted or complained about
//   - a symlink or junction is not ours either, whatever is behind it: not
//     read, not written to, not retired, not deleted - wherever it sits,
//     folders/ or staging/ or retired/ themselves included (see IsOurs)
//   - an order file naming something that is gone simply skips it; anything
//     present that it doesn't name goes to the end
//   - a directory whose readable half was renamed by hand keeps its place,
//     because the trailing uid is what identifies it
//   - two directories claiming one id - which is what copying one in a file
//     manager produces - is not corruption: the second gets a fresh id and
//     is thereafter a canvas of its own
//   - a currentCanvasId naming nothing falls back to a canvas that exists,
//     rather than the whole library refusing to open
//
// The same reconciliation is what makes a half-finished save survivable: a
// crash mid-write leaves the same kind of inconsistency a hand-edit does.
//
// Scope: the app owns the tree while it is running. Rearranging it under a
// live instance is undefined until the next start - there is no watcher,
// and promising live pickup would make every save path much harder.
//
// A snippet's pictures live in the snippet's own directory, so that moving
// one snippet is moving one directory - record, capture and thumbnail
// together, with no window in which the record has moved and the picture
// has not. It also removes a whole mechanism: there is no longer a shared
// image directory to scan for files whose item has gone, because deleting
// the snippet deletes its bytes.
//
// Layer::imageFile still names a file rather than a path, deliberately: a
// name with its location baked in would have to be rewritten every time its
// snippet moved. FindImage is the one place that turns a name into a path,
// and it needs the owning snippet to do it: the file is in that snippet's
// directory, wherever the directory currently is.
//
// **What a save retires is exactly what it once read, and nothing else -
// and retiring is setting aside, not deleting.** Load indexes every
// directory whose record it could read, and Save moves those the library no
// longer holds into retired/ - after placing everything it does hold, so a
// snippet moved between canvases is moved and not retired on the way. A
// directory Load skipped (no record, or one it could not read) is not in
// the index and cannot be retired. A save deletes no directory at all:
// deleting for good is Remove, at the moment it is asked for. See Save's
// own comment.
//
// **A deleted thing is an ordinary record.** A delete marks a folder,
// canvas or snippet with a deletedAt stamp and leaves it where it is (see
// CanvasManager's class comment), so it is saved, loaded and moved about by
// the same code as everything else, and a delete touches nothing on disk
// but the stamp in its record.
class LibraryStore {
public:
    explicit LibraryStore(std::filesystem::path rootDir);

    // The shape of the library this build reads and writes, stamped into
    // library.json by every save. It goes up whenever a build writes
    // something an older one would misread or drop - a new field in a
    // record, a new kind of file.
    static constexpr int kFormatVersion = 1;
    // Whether library.json says a newer build wrote this library: a
    // version above kFormatVersion. Such a library is not this build's to
    // open - every record it rewrote would lose what the newer build put
    // there - so once this has been seen, by this or by Load, the store
    // writes nothing at all: Save, Remove, SaveImage and SaveThumbnail all
    // fail. TrayController::Initialize asks before loading, and refuses to
    // start. A library.json that is missing, unreadable or has no version
    // is not newer.
    bool WrittenByANewerVersion() const;

    // Where the library lives - for a diagnostic that shows the tree as it
    // is on disk (see OverlayApp::DrawLibraryTreeHud).
    const std::filesystem::path& RootDir() const { return rootDir_; }
    // Bumped by every write this store makes to the disk - a save, whatever
    // it wrote or failed to write; a picture; a thumbnail; a file set aside
    // on load - so that something mirroring the tree knows when to look
    // again without every caller having to say so.
    uint64_t WriteGeneration() const { return writeGeneration_; }

    // Deletes the directory of the folder, canvas or snippet `uid` names,
    // with everything of the library's inside it, for good and now - what
    // "Delete permanently" is on disk, once the model no longer holds the
    // thing. Now rather than at the next save, which would take a
    // directory the model has lost for something gone missing, and set it
    // aside. Only what the store writes goes (see RemoveOwnDirectory):
    // anything someone else put beside a record stays, and the directory
    // stands for it, holding no record - which nothing reads back.
    //
    // True only once the directory is gone. False if this store knows no
    // directory for it - a thing never saved has none, and a capture of one
    // still in staging is collected by the next save - or if something in
    // it could not be removed: a picture held open by another program, on
    // Windows. That directory is then marked on disk as removed (a
    // `.removed` file in it), dropped from the index, and remembered as
    // owed: every save from then on takes another run at removing it, and
    // a Load that finds the mark reads nothing from the directory and owes
    // the removal too, so a restart cannot bring back what was deleted for
    // good. The caller can tell the two falses apart with
    // HasPendingRemoval.
    //
    // `remaining` is the library without it, which is what says whether
    // anything under the directory has been moved out in the model and not
    // yet on disk - a snippet moved to another canvas, a canvas out of a
    // folder, before the save that moves its directory. Deleting the
    // directory now would take that with it. The removal is then owed
    // instead, with nothing touched and no mark written - the mark would
    // hide what was moved from a restart - and the next save, which places
    // what was moved before it runs the removals owed, finishes it once
    // nothing the library holds is left inside.
    bool Remove(uint64_t uid, const LibraryView& remaining) const;
    bool Remove(uint64_t uid, const CanvasManagerSnapshot& remaining) const {
        return Remove(uid, LibraryView{remaining.folders, remaining.canvases, remaining.currentFolderId,
                                       remaining.currentCanvasId});
    }
    // Whether a Remove of `uid` is still owed: it was asked for and the
    // directory is still there, whole or in part.
    bool HasPendingRemoval(uint64_t uid) const { return pendingRemovals_.count(uid) > 0; }
    // Whether any removal is still owed - what keeps a session's "unsaved
    // changes" true until the disk agrees with the library about what is
    // gone, so that the retry is asked for rather than waited on.
    bool HasPendingRemovals() const { return !pendingRemovals_.empty(); }

    // Loads the on-disk library, or returns nullopt only if `rootDir` has
    // none at all - no library.json *and* no folders/ tree - which is a
    // first run (see TrayController::Initialize, which falls back to
    // CanvasManager's own freshly-constructed default state). Or if a newer
    // build wrote it (see WrittenByANewerVersion), which reads nothing and
    // leaves the store writing nothing, so that a fresh start over it
    // cannot save over the tree.
    //
    // The tree is the library; library.json is a pointer file beside it.
    // So a library.json that is missing or not JSON costs the pointers it
    // held and nothing else: the tree is
    // read as usual and the pointers are repaired from it. Reporting the
    // library absent over its pointer file would start the app fresh, and
    // a fresh library saved over a tree is how a tree gets retired.
    //
    // Nothing in the *content* of the files can make this throw - the same
    // "bad input means defaults" rule AppConfig::ParseConfig keeps; every
    // filesystem call takes an error_code. Running out of memory can, as
    // it can anywhere, which is what the size budgets on what is read are
    // for (see the .cpp). A field of the wrong
    // type in library.json reads as its default; a folder, canvas or
    // snippet record that cannot be read is skipped, and skipped is all
    // it is (see the class comment on what Save may retire). Everything
    // dangling is repaired rather than refused - see the .cpp.
    std::optional<CanvasManagerSnapshot> Load() const;

    // Writes the library to the tree - every record whose content changed
    // since this store last wrote it, each via a temp-file-then-rename so a
    // reader never observes a half-written file (see WriteFileAtomically) -
    // then sets aside, into retired/, the directories of whatever the
    // library no longer holds, and moves freshly captured pictures out of
    // staging into their snippets' directories. The .cpp's comment on Save
    // is the plan.
    //
    // Returns true only if every record the library holds is on disk as
    // written: library.json, every folder, canvas and snippet record, and
    // every order file. A false means something is not, and the caller must
    // not treat the snapshot as saved - the next Save retries exactly what
    // failed. Moving a picture and retiring a directory are best-effort and
    // outside the result: a picture not yet moved is still readable where
    // it is, a directory not yet retired costs disk space, and the next save
    // takes another run at both.
    bool Save(const LibraryView& view) const;
    // The snippets the last Save could not write because their record
    // would be larger than Load reads (see kMaxRecordBytes) - so that a UI
    // can say why the save failed. Empty after a save that had none.
    const std::set<uint64_t>& OversizedRecords() const { return oversizedRecords_; }
    bool Save(const CanvasManagerSnapshot& snapshot) const {
        return Save(LibraryView{snapshot.folders, snapshot.canvases, snapshot.currentFolderId,
                                snapshot.currentCanvasId});
    }

    // Encodes `pixelsRGBA` (width*height*4 bytes RGBA8, row-major,
    // top-left origin) as "<itemId>.qoi" in the snippet's own directory -
    // or in staging, if the snippet has never been saved and has no
    // directory yet (see ImageHome) - and returns the filename to store in
    // Layer::imageFile, or nullopt on failure. Called synchronously right
    // after a successful capture (see Session::CaptureShotItem),
    // independent of Save() itself - so a captured screenshot survives
    // even a crash that never reaches the next debounced metadata save.
    // Synchronously is also why the format matters: this runs on the
    // render thread while the user waits, and QOI encodes the same pixels
    // in a twentieth of the time PNG took (see EncodeQoiToFile).
    std::optional<std::string> SaveImage(uint64_t itemId, const uint8_t* pixelsRGBA, int width, int height) const;

    // The same for a painted layer's pixels, as "<itemId>_p<layerIndex>.qoi":
    // an item can have several layers with pixels of their own (a
    // screenshot with something painted over it), and the id alone no
    // longer names a file uniquely. The store names the file rather than
    // the caller, so that every picture's name says which snippet it
    // belongs to, even where no record does.
    std::optional<std::string> SaveLayerImage(uint64_t itemId, size_t layerIndex, const uint8_t* pixelsRGBA,
                                               int width, int height) const;

    // Decodes a previously-saved picture of snippet `itemId` (see
    // SaveImage/Layer::imageFile) back into raw pixels, for reloading its
    // texture when its canvas becomes current. Returns nullopt for an
    // empty filename or if the file is missing/undecodable. The snippet is
    // what says where to look - see FindImage.
    std::optional<DecodedImage> LoadImage(uint64_t itemId, const std::string& filename) const;
    // Whether that picture is there at all - what tells a picture that
    // could not be read (held open, say) from one that does not exist.
    bool HasImage(uint64_t itemId, const std::string& filename) const;

    // The longest edge a thumbnail is written at. A canvas tile in the
    // Overview is 200x130 and an item inside one is smaller still, so this
    // is already generous; what it buys is the difference between a ~40 KB
    // file that decodes in well under a millisecond and the 8 MB a
    // fullscreen capture decodes to.
    static constexpr int kThumbnailMaxExtent = 256;

    // "<stem>.thumb.qoi" for an image named "<stem>.qoi". Public because Save()'s
    // own GC has to recognize these, and because it is the one thing a test
    // needs to look at the file directly.
    static std::string ThumbnailFilename(const std::string& imageFilename);

    // Writes a thumbnail beside `imageFilename`, downscaling `image` to
    // kThumbnailMaxExtent first if it is bigger. Best-effort: a thumbnail
    // that can't be written costs a slow first look at the Overview, not
    // correctness, so nothing that saves an image cares whether this
    // succeeded.
    //
    // Called for every picture written (see WritePicture), and from the
    // Overview for a picture whose thumbnail is missing - which puts it
    // back, one visit at a time.
    bool SaveThumbnail(uint64_t itemId, const std::string& imageFilename, const DecodedImage& image) const;

    // The thumbnail for snippet `itemId`'s `imageFilename`, or nullopt if
    // there isn't one - which is not an error: the caller falls back to
    // decoding the full image, and writes the thumbnail on the way.
    std::optional<DecodedImage> LoadThumbnail(uint64_t itemId, const std::string& imageFilename) const;

private:
    // Where a snippet's pictures go and where they are found. A snippet's
    // pictures live in its own directory, which is what lets one be moved
    // by moving one directory - but Layer::imageFile names a file, not a
    // path, and deliberately so: an image that had its location baked into
    // its name would have to be rewritten every time its snippet moved. So
    // the snippet's id is the other half of every lookup: its directory
    // (itemDirs_) is where its pictures are, and staging is where they wait
    // while it has no directory yet.
    std::filesystem::path ImageHome(uint64_t itemId) const;
    std::filesystem::path FindImage(uint64_t itemId, const std::string& filename) const;
    // Behind SaveImage and SaveLayerImage: encodes into the snippet's home
    // under `filename`, with a thumbnail beside it.
    std::optional<std::string> WritePicture(uint64_t itemId, const std::string& filename, const uint8_t* pixelsRGBA,
                                             int width, int height) const;

    // Whether `path` names something inside rootDir_, lexically. Every path
    // this store deletes is checked against it first - a tripwire, since
    // every path it could delete was built under the root to begin with.
    bool WithinRoot(const std::filesystem::path& path) const;
    // Whether `path` is a place this store may write into, move or delete:
    // under the root, and with no link or junction anywhere on the way
    // down from the root to it. The one check every write, move and
    // delete goes through - the top-level directories included (folders/,
    // staging/, retired/), which a user can replace with a junction as
    // easily as any other. The library root itself is not checked: a root
    // that is a junction is how a library is moved to another drive, and
    // is supported. A path that does not exist yet passes if its existing
    // ancestors do, which is what a directory about to be created needs.
    // See the .cpp on links.
    bool IsOurs(const std::filesystem::path& path) const;
    // Drops every index entry that points under `dir` - what a removed or
    // retired directory takes with it.
    // Entries whose id is in `keep` stay.
    void ForgetUnder(const std::filesystem::path& dir, const std::unordered_set<uint64_t>& keep = {}) const;
    // Whether a directory of anything in `ids` is indexed under `dir`.
    bool HoldsAnyOf(const std::filesystem::path& dir, const std::unordered_set<uint64_t>& ids) const;
    // Where the folder directories are: folders/ under the root.
    std::filesystem::path FoldersRoot() const;
    // The one way this store deletes a directory: what the store itself
    // writes, recursively through the directories that hold its records,
    // and only if it IsOurs, so that nothing outside the library is ever
    // emptied through something pointing at it. A directory goes once it
    // is empty; one that someone else's files keep stays for them. True
    // once nothing of the library's is left under `path`.
    bool RemoveOwnDirectory(const std::filesystem::path& path) const;
    // Puts the removed mark into `dir` - a directory RemoveOwnDirectory
    // could not finish with - so that the intent outlives the process.
    // True once the mark is there.
    bool MarkRemoved(const std::filesystem::path& dir) const;
    // Whether `dir` carries the removed mark; if so it is remembered as
    // owed (by the uid its name ends in) and nothing in it is to be read.
    bool NotePendingRemoval(const std::filesystem::path& dir) const;

    // The tree walk behind Load: every folder, canvas and snippet under
    // `foldersRoot`, into `out`, indexing each directory and noting each
    // record that came back exactly as a save would write it.
    void ReadTree(const std::filesystem::path& foldersRoot, CanvasManagerSnapshot& out) const;
    // Rebuilds folderDirs_/canvasDirs_/itemDirs_ by reading the tree - what
    // every save used to do, now only done when there is no index to trust
    // (a store that has never loaded, i.e. a first run). Save keeps only
    // the entries for what it is about to write: a store that never read a
    // directory has no standing to retire it - see Save.
    void IndexTreeFromDisk(const std::filesystem::path& foldersRoot) const;

    // ===== What makes a save incremental =====
    //
    // A save used to cost the whole library however little had changed, and
    // measured on a 12-canvas library that was a full second on the render
    // thread. It went three ways, in roughly these proportions: a third
    // re-reading and re-parsing every file on disk to find out which
    // directory held which id, half re-serializing every record, and the
    // rest actually writing them. All three are avoided below, and all three
    // had to be, since fixing any one alone leaves most of the second.
    //
    // Where each thing lives, so a save doesn't have to go and look. Filled
    // in by Load, which walks the whole tree anyway, and maintained by Save
    // as it places, renames and collects directories.
    //
    // `treeIndexed_` false means these are not to be trusted - a store that
    // has never loaded (a first run), or one whose save failed partway - and
    // the next Save rebuilds them from disk the slow way. Being wrong is
    // survivable rather than corrupting: a directory the index has lost is
    // written afresh under its proper name and the stale one is collected by
    // the same GC pass that handles a deleted snippet.
    mutable std::map<uint64_t, std::filesystem::path> folderDirs_;
    mutable std::map<uint64_t, std::filesystem::path> canvasDirs_;
    mutable std::map<uint64_t, std::filesystem::path> itemDirs_;
    mutable bool treeIndexed_ = false;
    // What Remove was asked to delete and could not, wholly, and where it
    // is - see Remove. Each save tries again. Not in the index, so that
    // nothing under it is placed, retired or read meanwhile: it was
    // deleted, not lost.
    mutable std::map<uint64_t, std::filesystem::path> pendingRemovals_;
    // See OversizedRecords.
    mutable std::set<uint64_t> oversizedRecords_;
    // See WriteGeneration.
    mutable uint64_t writeGeneration_ = 0;
    // See WrittenByANewerVersion: set once a newer library has been seen,
    // and never cleared.
    mutable bool writtenByANewerVersion_ = false;

    // What was last written, so a save can tell what has actually changed.
    //
    // Items carry a content hash rather than their text: they are where all
    // the bulk is (a stroke point is a JSON object, and an ordinary canvas
    // has tens of thousands of them), so the whole point is to answer
    // "changed?" *without* serializing. Everything else keeps the exact text
    // it wrote, because those records are a handful of scalars - serializing
    // one to compare it costs nothing, and an exact comparison has no
    // question of coverage hanging over it.
    //
    // Load fills these in too, record by record, so the first save of a
    // session costs what changed like every save after it. Each record that
    // came back exactly as a save would write it is noted as written;
    // anything the load had to repair - an id reassigned because two
    // directories claimed it, an order file that disagreed with the
    // directories beside it - is left out and therefore written. Making
    // the first save a full one instead costs seconds on a large library
    // for the sake of exactly those repaired records.
    //
    // Missing from these is never wrong, only slower: a record not noted is
    // written. Present and stale would be wrong, which is why a failed
    // write erases its entry and a retired record's entries go with it.
    mutable std::unordered_map<uint64_t, uint64_t> writtenItemHashes_;
    mutable std::unordered_map<std::string, std::string> writtenFileText_;

    std::filesystem::path rootDir_;
};

}  // namespace sz::core::persistence
