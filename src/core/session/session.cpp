#include "core/session/session.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include "core/canvas/item_geometry.h"
#include "core/util/timestamp_name.h"

namespace sz::core {

namespace {
// See Session::UpdateAutosave: a write happens once content has been quiet
// for kAutosaveQuietSeconds (coalescing a burst of edits - dragging, a long
// stroke - into one save instead of many), or after
// kAutosaveMaxIntervalSeconds regardless, so a very long uninterrupted
// editing session still gets persisted periodically rather than only once
// activity finally pauses.
constexpr float kAutosaveQuietSeconds = 2.0f;
constexpr float kAutosaveMaxIntervalSeconds = 15.0f;
// A save that failed is tried again, but on its own clock: the first retry
// waits the quiet period, each one after waits twice as long as the last,
// up to this. Without a clock of its own the retry fell through to the
// quiet-period check, which a failed save does nothing to reset - so a full
// disk turned an ordinary autosave into a synchronous rewrite of every file
// on every frame, for as long as the disk stayed full.
constexpr float kAutosaveRetryMaxSeconds = 30.0f;
}  // namespace

// ================= Persistence =================

bool Session::SavePendingPictures(LibraryInstance& instance) {
    if (!instance.store) {
        return true;
    }
    bool wroteEverything = true;
    for (auto it = pendingPictures_.begin(); it != pendingPictures_.end();) {
        Item* item = instance.manager.FindItemAnywhere(it->first);
        if (!item) {
            it = pendingPictures_.erase(it);  // erased for good since; nothing to keep it for
            continue;
        }
        const PendingPicture& pending = it->second;
        if (instance.store->SaveImage(it->first, pending.pixelsRGBA.data(), pending.width, pending.height)) {
            item->picture.stored = true;
            it = pendingPictures_.erase(it);
        } else {
            wroteEverything = false;
            ++it;
        }
    }
    return wroteEverything;
}

bool Session::WriteRecoveryCopy(const std::filesystem::path& file) {
    // The library as it was last saved, pictures and all, when it can still
    // be read - then what this session holds, saved over it. Load first, so
    // that the save knows what is there and takes out what the session no
    // longer holds.
    const bool copiedLibrary = Store() != nullptr && Store()->WriteCopyTo(file);
    persistence::LibraryStore copy(file);
    if (copiedLibrary) {
        copy.Load();
    }
    const bool recordsWritten = copy.Save(Model().View());
    // Every picture: a capture whose write never landed from this session,
    // the rest with the library it was copied from. The first version
    // copied only what was in memory and left every screenshot out, so the
    // copy opened with placeholders and nothing to say why.
    size_t picturesMissing = 0;
    for (const Canvas& canvas : Model().Canvases()) {
        for (const Item& item : canvas.items) {
            if (const auto pending = pendingPictures_.find(item.id); pending != pendingPictures_.end()) {
                if (!copy.SaveImage(item.id, pending->second.pixelsRGBA.data(), pending->second.width,
                                    pending->second.height)) {
                    ++picturesMissing;
                }
            } else if (item.picture.stored && !copy.HasImage(item.id)) {
                ++picturesMissing;
            }
        }
    }
    // A note beside it for the person who finds it: what it is, where it
    // came from, and whether every picture came with it.
    {
        std::filesystem::path notePath = file;
        notePath += ".txt";
        std::ofstream note(notePath, std::ios::binary | std::ios::trunc);
        note << "Spickzettel recovery copy, written " << TimestampName() << "\n";
        note << "Source library: " << (Store() ? Store()->File().string() : std::string("(none)")) << "\n";
        if (picturesMissing == 0 && recordsWritten) {
            note << "Complete: everything the app held, and every picture of it, is in this file.\n";
        } else {
            note << "Incomplete: " << picturesMissing << " picture(s) could not be copied"
                 << (recordsWritten ? "" : ", and the library itself could not be written") << ".\n";
        }
        note << "To use it, close the app and put this file where the source library was, under its name.\n";
    }
    return recordsWritten && picturesMissing == 0;
}

bool Session::HasUnsavedChanges() const {
    return library_.manager.Generation() != library_.lastSavedGeneration || !pendingPictures_.empty();
}

void Session::SyncTexturesToCurrentCanvas() {
    if (!Store() || !window_) {
        return;
    }
    Model().SyncShotTexturesToCanvas(
        Model().CurrentCanvasId(),
        [this](const Item& item) -> uint64_t {
            const std::optional<persistence::DecodedImage> decoded = Store()->LoadImage(item.id);
            if (!decoded.has_value()) {
                return 0;
            }
            return window_->CreateTextureFromPixels(decoded->pixelsRGBA.data(), decoded->width, decoded->height);
        },
        [this](uint64_t texture) { window_->ReleaseTexture(texture); });
    shotTextureCanvasId_ = Model().CurrentCanvasId();
}

void Session::EnsureTexturesForCurrentCanvas() {
    if (shotTextureCanvasId_ != Model().CurrentCanvasId()) {
        SyncTexturesToCurrentCanvas();
    }
}

void Session::ReplaceLostTextures() {
    if (!window_) {
        return;
    }
    for (Canvas& canvas : Model().CanvasesMutable()) {
        for (Item& item : canvas.items) {
            Picture& picture = item.picture;
            if (picture.textureHandle == 0) {
                continue;
            }
            window_->ReleaseTexture(picture.textureHandle);
            picture.textureHandle = 0;
            // Not stored yet, so the sync has nothing to load it from: the
            // pixels waiting to be written are what it showed.
            const auto pending = pendingPictures_.find(item.id);
            if (!picture.stored && pending != pendingPictures_.end()) {
                picture.textureHandle = window_->CreateTextureFromPixels(
                    pending->second.pixelsRGBA.data(), pending->second.width, pending->second.height);
            }
        }
    }
    shotTextureCanvasId_.reset();
    // Frozen with or without a texture - a freeze made while the device was
    // gone has only its pixels - it has one from here.
    if (!frozenScreenPixels_.empty()) {
        if (frozenScreenTexture_ != 0) {
            window_->ReleaseTexture(frozenScreenTexture_);
        }
        frozenScreenTexture_ =
            window_->CreateTextureFromPixels(frozenScreenPixels_.data(), frozenScreenWidth_, frozenScreenHeight_);
        if (frozenScreenTexture_ == 0) {
            ReleaseFrozenScreen();  // nothing frozen, as when the capture itself fails
        }
    }
}

void Session::Tick(float deltaSeconds) { UpdateAutosave(library_, deltaSeconds); }

void Session::UpdateAutosave(LibraryInstance& instance, float deltaSeconds) {
    if (!instance.store) {
        return;
    }

    const uint64_t generation = instance.manager.Generation();
    if (generation != instance.lastObservedGeneration) {
        instance.lastObservedGeneration = generation;
        instance.secondsSinceLastChange = 0.0f;
    } else {
        instance.secondsSinceLastChange += deltaSeconds;
    }

    if (!HasUnsavedChanges()) {
        instance.secondsSinceFirstUnsavedChange = 0.0f;
        return;  // nothing pending
    }
    instance.secondsSinceFirstUnsavedChange += deltaSeconds;

    // A failed save waits its turn rather than the content's. Fresh edits
    // in the meantime keep counting toward the quiet period as usual, and
    // are picked up by the retry when it comes.
    if (instance.saveRetryCountdownSeconds > 0.0f) {
        instance.saveRetryCountdownSeconds -= deltaSeconds;
        if (instance.saveRetryCountdownSeconds > 0.0f) {
            return;
        }
    }

    const bool quietLongEnough = instance.secondsSinceLastChange >= kAutosaveQuietSeconds;
    const bool waitedTooLong = instance.secondsSinceFirstUnsavedChange >= kAutosaveMaxIntervalSeconds;
    if (quietLongEnough || waitedTooLong) {
        SaveLibraryNow(instance);
    }
}

bool Session::SaveLibraryNow(LibraryInstance& instance) {
    if (!instance.store) {
        return true;
    }
    // Pixels first, then the metadata that points at them. Both have to
    // land for the save to count: the generation is acknowledged only when
    // everything it covers is on disk, and a capture's pixels still waiting
    // to be written are covered by it as much as the record that names
    // them. The metadata is written even when a picture wasn't - what did
    // land is worth having, and the whole thing is retried until all of it
    // has.
    const uint64_t generation = instance.manager.Generation();
    const bool picturesSaved = SavePendingPictures(instance);
    const bool metadataSaved = instance.store->Save(instance.manager.View());
    const bool saved = picturesSaved && metadataSaved;
    if (saved) {
        instance.lastSavedGeneration = generation;
        instance.saveRetryBackoffSeconds = 0.0f;
    } else {
        // Disk full, permissions, a file held open by something else: try
        // again later, and later each time - see kAutosaveRetryMaxSeconds.
        instance.saveRetryBackoffSeconds = instance.saveRetryBackoffSeconds <= 0.0f
                                               ? kAutosaveQuietSeconds
                                               : std::min(instance.saveRetryBackoffSeconds * 2.0f, kAutosaveRetryMaxSeconds);
        instance.saveRetryCountdownSeconds = instance.saveRetryBackoffSeconds;
    }
    instance.secondsSinceFirstUnsavedChange = 0.0f;
    return saved;
}

bool Session::FlushIfDirty(LibraryInstance& instance) {
    if (!instance.store || !HasUnsavedChanges()) {
        return true;
    }
    return SaveLibraryNow(instance);
}

bool Session::Flush() { return FlushIfDirty(library_); }

// ================= Changes that are not undone =================

void Session::SwitchToCanvas(CanvasId id) {
    if (id == Model().CurrentCanvasId()) {
        return;
    }
    EndOpenGesture();
    Model().SwitchToCanvas(id);
    if (Model().CurrentCanvasId() == id) {
        liveLayer_.Clear();
    }
}

void Session::SwitchToFolder(FolderId id) {
    EndOpenGesture();
    Model().SwitchToFolder(id);
}

CanvasId Session::AddCanvas(std::string name) {
    EndOpenGesture();
    const CanvasId before = Model().CurrentCanvasId();
    const CanvasId id = Model().AddCanvas(std::move(name));
    // The first canvas of an empty library is current as it is made.
    if (Model().CurrentCanvasId() != before) {
        liveLayer_.Clear();
    }
    return id;
}

FolderId Session::AddFolder(std::string name) {
    EndOpenGesture();
    return Model().AddFolder(std::move(name));
}

void Session::RenameFolder(FolderId id, std::string name) {
    EndOpenGesture();
    Model().RenameFolder(id, std::move(name));
}

void Session::RenameCanvas(CanvasId id, std::string name) {
    EndOpenGesture();
    Model().RenameCanvas(id, std::move(name));
}

void Session::ReorderFolder(FolderId id, size_t newIndex) {
    EndOpenGesture();
    Model().ReorderFolder(id, newIndex);
}

void Session::ReorderCanvas(CanvasId id, size_t newIndex) {
    EndOpenGesture();
    Model().ReorderCanvas(id, newIndex);
}

void Session::MoveCanvasToFolder(CanvasId canvasId, FolderId folderId) {
    EndOpenGesture();
    Model().MoveCanvasToFolder(canvasId, folderId);
}

void Session::SetPinned(const std::vector<ItemId>& ids, bool pinned) {
    EndOpenGesture();
    bool changed = false;
    for (const ItemId id : ids) {
        if (Item* item = Model().FindItemAnywhere(id); item != nullptr && item->pinned != pinned) {
            item->pinned = pinned;
            changed = true;
        }
    }
    if (changed) {
        Model().MarkChanged();
    }
}

void Session::SetMinimized(const std::vector<ItemId>& ids, bool minimized) {
    EndOpenGesture();
    bool changed = false;
    for (const ItemId id : ids) {
        if (Item* item = Model().FindItemAnywhere(id); item != nullptr && item->minimized != minimized) {
            item->minimized = minimized;
            changed = true;
        }
    }
    if (changed) {
        Model().MarkChanged();
    }
}

void Session::BringItemsToFront(const std::vector<ItemId>& ids) {
    EndOpenGesture();
    Model().BringItemsToFront(ids);
}

void Session::MoveItemLayer(ItemId id, int direction) {
    EndOpenGesture();
    Model().MoveItemLayer(id, direction);
}

void Session::SyncItemsToDisplaySize(float width, float height) { Model().SyncItemsToDisplaySize(width, height); }

// ================= Deleting and restoring =================

void Session::ImportLibrary(CanvasManagerSnapshot snapshot) {
    library_.manager.ImportSnapshot(std::move(snapshot));
    // Without DeletePermanently's texture sync: this runs before the overlay
    // window has made its device, so the sync could load nothing - and would
    // still record the current canvas as loaded, leaving its pictures as
    // placeholders until the canvas changed. Nothing is resident yet for the
    // erasing to give back; the first frame loads what is there.
    for (const ItemId id : Model().MarkedSnippets()) {
        Erase(id);
    }
}

bool Session::Delete(uint64_t id) {
    EndOpenGesture();
    if ((Model().FindFolder(id) == nullptr && Model().FindCanvas(id) == nullptr) ||
        !Model().MarkDeleted(id, static_cast<int64_t>(std::time(nullptr)))) {
        return false;
    }
    // Hidden now - and what leaves the screen gives its pictures back in the
    // same frame, as anything leaving it does. The gated form would see the
    // same canvas current and do nothing.
    SyncTexturesToCurrentCanvas();
    return true;
}

bool Session::Restore(uint64_t id) {
    EndOpenGesture();
    if ((Model().FindFolder(id) == nullptr && Model().FindCanvas(id) == nullptr) || !Model().Restore(id)) {
        return false;
    }
    SyncTexturesToCurrentCanvas();
    return true;
}

bool Session::DeletePermanently(uint64_t id) {
    EndOpenGesture();
    if (!Erase(id)) {
        return false;
    }
    SyncTexturesToCurrentCanvas();
    // Out of the library now rather than at the next autosave, so that
    // nothing deleted for good comes back after a crash in between.
    SaveLibraryNow(library_);
    return true;
}

bool Session::Erase(uint64_t id) {
    CanvasManager& manager = Model();
    // What goes with it: the textures of every snippet under it, and every
    // change about any of them on any canvas's history, with the stacks of
    // every canvas that goes and every move from or to one (see
    // history::History::ForgetCanvas). The library loses it at the next
    // save, which writes what the model holds.
    const bool isFolder = manager.FindFolder(id) != nullptr;
    bool found = isFolder;
    for (Canvas& canvas : manager.CanvasesMutable()) {
        const bool wholeCanvas = canvas.id == id || (isFolder && canvas.folderId == id);
        if (wholeCanvas) {
            found = true;
            history_.ForgetCanvas(canvas.id);
        }
        for (Item& item : canvas.items) {
            if (!wholeCanvas && item.id != id) {
                continue;
            }
            found = true;
            history_.ForgetItem(item.id);
            if (item.picture.textureHandle != 0 && window_) {
                window_->ReleaseTexture(item.picture.textureHandle);
            }
            item.picture.textureHandle = 0;
        }
    }
    return found && manager.Erase(id);
}

bool Session::DeleteMarkedCanvasesPermanently(FolderId folderId) {
    EndOpenGesture();
    bool any = false;
    for (const CanvasId id : Model().MarkedCanvasesIn(folderId)) {
        any = Erase(id) || any;
    }
    if (any) {
        SyncTexturesToCurrentCanvas();
        SaveLibraryNow(library_);
    }
    return any;
}

size_t Session::EraseDeletedBefore(int64_t cutoff) {
    size_t erased = 0;
    for (const uint64_t id : Model().MarkedBefore(cutoff)) {
        if (Erase(id)) {
            ++erased;
        }
    }
    return erased;
}

// ================= Capturing the screen =================

void Session::FreezeScreen(const platform::DisplayInfo& display) {
    ReleaseFrozenScreen();
    if (!window_ || display.width <= 0 || display.height <= 0) {
        return;
    }
    // The whole display the overlay is on. The window is sized to it, and
    // the capture excludes the overlay's own content (see
    // IOverlayWindow::CaptureRegionAsTexture), so what comes back is the
    // application underneath and nothing of ours.
    platform::CaptureResult capture = window_->CaptureRegionAsTexture(
        platform::Rect{0.0f, 0.0f, static_cast<float>(display.width), static_cast<float>(display.height)});
    // A zero handle is the ordinary "couldn't" answer - a backend without
    // capture, or a display the OS won't hand over - and it needs no
    // special case: with nothing frozen the overlay just stays transparent,
    // exactly as it behaves with the setting off.
    frozenScreenTexture_ = capture.textureHandle;
    frozenScreenWidth_ = capture.width;
    frozenScreenHeight_ = capture.height;
    // Kept for CaptureShotItem to crop out of. Moved rather than copied:
    // this is a full screen's worth of pixels and nothing else wants them.
    // Kept without a texture too - a device lost and not yet replaced - so
    // that the shots taken meanwhile are cut from what was frozen, and the
    // picture shows once the device is back (see ReplaceLostTextures).
    frozenScreenPixels_ = std::move(capture.pixelsRGBA);
}

// A sub-rectangle of the frozen screen, uploaded as its own texture and
// returned in the same shape a live capture would come back in - so the
// caller can treat the two identically. nullopt means "nothing frozen, go
// and capture", which covers the setting being off, the freeze having
// failed, and a backend that can't capture at all.
//
// The rect is in screen coordinates, which is also the frozen image's own
// coordinate space (it is the whole primary display), so the crop is a plain
// sub-rectangle with no scaling. It is still clamped: an item can be dragged
// partly off-screen, and the display could in principle have changed size
// since the freeze.
std::optional<platform::CaptureResult> Session::CropFrozenScreen(const Rect& rect) const {
    if (frozenScreenPixels_.empty() || !window_ || frozenScreenWidth_ <= 0 || frozenScreenHeight_ <= 0) {
        return std::nullopt;
    }
    const int left = std::clamp(static_cast<int>(std::floor(rect.x)), 0, frozenScreenWidth_);
    const int top = std::clamp(static_cast<int>(std::floor(rect.y)), 0, frozenScreenHeight_);
    const int right = std::clamp(static_cast<int>(std::ceil(rect.x + rect.w)), left, frozenScreenWidth_);
    const int bottom = std::clamp(static_cast<int>(std::ceil(rect.y + rect.h)), top, frozenScreenHeight_);
    const int width = right - left;
    const int height = bottom - top;
    if (width <= 0 || height <= 0) {
        return std::nullopt;  // entirely off-screen; a live capture would fail too
    }
    // Guards against a frozen buffer that doesn't match its own dimensions
    // rather than trusting them - this indexes raw memory.
    const size_t expected = static_cast<size_t>(frozenScreenWidth_) *
                            static_cast<size_t>(frozenScreenHeight_) * 4u;
    if (frozenScreenPixels_.size() < expected) {
        return std::nullopt;
    }

    platform::CaptureResult result;
    result.width = width;
    result.height = height;
    result.pixelsRGBA.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u);
    const size_t sourceStride = static_cast<size_t>(frozenScreenWidth_) * 4u;
    const size_t rowBytes = static_cast<size_t>(width) * 4u;
    for (int row = 0; row < height; ++row) {
        const size_t sourceOffset = (static_cast<size_t>(top + row) * sourceStride) +
                                     (static_cast<size_t>(left) * 4u);
        std::memcpy(result.pixelsRGBA.data() + static_cast<size_t>(row) * rowBytes,
                     frozenScreenPixels_.data() + sourceOffset, rowBytes);
    }
    // An upload that fails leaves the cut without a texture, as a live
    // capture's would: the pixels are what the user framed, and what is
    // saved. Captured live instead, the shot was of whatever the screen
    // showed by then.
    result.textureHandle = window_->CreateTextureFromPixels(result.pixelsRGBA.data(), width, height);
    return result;
}

void Session::ReleaseFrozenScreen() {
    if (frozenScreenTexture_ != 0 && window_) {
        window_->ReleaseTexture(frozenScreenTexture_);
    }
    frozenScreenTexture_ = 0;
    frozenScreenWidth_ = 0;
    frozenScreenHeight_ = 0;
    // shrink_to_fit, not just clear: a full-screen RGBA buffer is worth
    // handing back rather than keeping capacity for the next edit-mode
    // session, which re-captures anyway.
    frozenScreenPixels_.clear();
    frozenScreenPixels_.shrink_to_fit();
}

void Session::CaptureShotItem(Item& item) {
    Picture* picture = &item.picture;
    // Placeholder gradient seed, used as a fallback below (and always
    // needed if a later resize or a canvas-to-canvas copy loses the real
    // capture - see Picture::textureHandle's own doc comment).
    picture->placeholderHue = std::fmod(static_cast<float>(item.id) * 47.0f, 360.0f);

    if (!window_) {
        return;
    }
    // While the screen is frozen, crop the snippet out of the frozen image
    // rather than capturing the live screen again. Without this the two
    // disagree: the user drags a region over a still picture and gets back
    // whatever the game happened to be showing a moment later, which for a
    // moving camera is a different scene entirely. Falls through to a live
    // capture whenever nothing is frozen, which is also what happens if the
    // freeze itself failed.
    std::optional<platform::CaptureResult> cropped = CropFrozenScreen(item.rect);
    platform::CaptureResult result =
        cropped.has_value() ? std::move(*cropped)
                             : window_->CaptureRegionAsTexture(
                                   platform::Rect{item.rect.x, item.rect.y, item.rect.w, item.rect.h});
    if (result.textureHandle != 0) {
        picture->textureHandle = result.textureHandle;
    }
    // Written synchronously, independent of the debounced autosave - see
    // LibraryStore::SaveImage's own doc comment for why. No MarkChanged()
    // call needed here: the item this mutates was itself just created via
    // CanvasManager::CreateItem a moment earlier in the same synchronous
    // call stack, which already bumped the generation counter - nothing can
    // observe this item's state in between.
    //
    // A write that fails keeps the pixels rather than the texture alone:
    // what is on screen looks captured, and letting the only copy go would
    // make that a lie the next restart tells. They are tried again with
    // every save until they land, and no save counts until they have - see
    // SavePendingPictures.
    if (!result.pixelsRGBA.empty() && Store()) {
        if (Store()->SaveImage(item.id, result.pixelsRGBA.data(), result.width, result.height)) {
            picture->stored = true;
            pendingPictures_.erase(item.id);
        } else {
            pendingPictures_[item.id] = PendingPicture{std::move(result.pixelsRGBA), result.width, result.height};
        }
    }
}

bool Session::ClonePicturesForCopy(ItemId sourceId, ItemId copyId) {
    if (!Store()) {
        return true;
    }
    const Item* source = Model().FindItemAnywhere(sourceId);
    Item* copy = Model().FindItemAnywhere(copyId);
    if (!source || !copy) {
        return true;
    }
    const Picture* sourcePicture = &source->picture;
    Picture* copyPicture = &copy->picture;
    // The session's own copy of the pixels first: a capture whose write has
    // not landed is not in the library yet, and a copy taken of it in that
    // window used to come out with no picture at all, for good. Written for
    // the copy at once, and kept for the next save when that fails, as a
    // capture's is; its texture made here, since the texture sync loads
    // only what is stored.
    if (const auto pending = pendingPictures_.find(sourceId); pending != pendingPictures_.end()) {
        PendingPicture pixels = pending->second;
        if (window_) {
            copyPicture->textureHandle =
                window_->CreateTextureFromPixels(pixels.pixelsRGBA.data(), pixels.width, pixels.height);
        }
        if (Store()->SaveImage(copyId, pixels.pixelsRGBA.data(), pixels.width, pixels.height)) {
            copyPicture->stored = true;
        } else {
            pendingPictures_[copyId] = std::move(pixels);
        }
        return true;
    }
    if (!sourcePicture->stored) {
        return true;  // nothing to copy
    }
    // Copied as stored, without decoding it. The caller's texture sync
    // gives the copy its texture.
    if (!Store()->CopyImage(sourceId, copyId)) {
        return false;  // the picture could not be copied: the copy lacks it
    }
    copyPicture->stored = true;
    // No MarkChanged() call needed - same reasoning as CaptureShotItem's
    // own: this runs synchronously right after the copy itself
    // (CanvasManager::DuplicateItem/MoveOrCopyItemToCanvas), which already
    // bumped the generation counter moments earlier in the same call stack.
    return true;
}

}  // namespace sz::core
