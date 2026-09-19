#include "core/session/session.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>
#include <string>
#include <utility>
#include <vector>

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

bool Session::SavePaintedLayers(LibraryInstance& instance) {
    if (!instance.store) {
        return true;
    }
    bool wroteEverything = true;
    // Every painted layer on every canvas, not just the current one: a
    // layer painted on canvas A and then switched away from still has its
    // pixels in memory and nothing on disk until this runs.
    for (Canvas& canvas : instance.manager.CanvasesMutable()) {
        for (Item& item : canvas.items) {
            for (size_t index = 0; index < item.layers.size(); ++index) {
                Layer& layer = item.layers[index];
                if (!layer.HasPaintedPixels()) {
                    continue;
                }
                // Only what has actually changed. Encoding a fullscreen
                // layer is 13ms, and this now runs on every canvas switch
                // as well as every autosave - re-writing pixels that are
                // already on disk would make switching canvases cost real
                // time for nothing.
                if (!layer.paintedDirty) {
                    continue;
                }
                // Named by the store for the item and the layer's place in
                // its stack, so two painted layers on one item can't
                // collide. A layer that moves in the stack is written under
                // a new name and the old file is collected by Save's own GC.
                if (const std::optional<std::string> filename =
                        instance.store->SaveLayerImage(item.id, index, layer.painted->PixelsRGBA().data(),
                                                       layer.painted->Width(), layer.painted->Height())) {
                    layer.imageFile = *filename;
                    layer.paintedDirty = false;
                } else {
                    // A failed write leaves it dirty on purpose: these
                    // pixels are still the only copy, and the sync below
                    // refuses to drop a layer that is still the only copy
                    // of itself. And it is reported, so the save that
                    // called this is not acknowledged either - a layer
                    // that stayed dirty behind an acknowledged save was
                    // never retried until something unrelated changed.
                    wroteEverything = false;
                }
            }
        }
    }
    return wroteEverything;
}

bool Session::SavePendingPictures(LibraryInstance& instance) {
    if (!instance.store) {
        return true;
    }
    bool wroteEverything = true;
    for (auto it = pendingPictures_.begin(); it != pendingPictures_.end();) {
        Item* item = instance.manager.FindItemAnywhere(it->first);
        Layer* picture = item ? item->ImageLayer() : nullptr;
        if (!picture) {
            it = pendingPictures_.erase(it);  // erased for good since; nothing to keep it for
            continue;
        }
        const PendingPicture& pending = it->second;
        if (const std::optional<std::string> filename =
                instance.store->SaveImage(it->first, pending.pixelsRGBA.data(), pending.width, pending.height)) {
            picture->imageFile = *filename;
            it = pendingPictures_.erase(it);
        } else {
            wroteEverything = false;
            ++it;
        }
    }
    return wroteEverything;
}

bool Session::WriteRecoveryCopy(const std::filesystem::path& dir) {
    persistence::LibraryStore copy(dir);
    CanvasManagerSnapshot snapshot = library_.manager.ExportSnapshot();
    bool wroteEverything = true;
    for (Canvas& canvas : snapshot.canvases) {
        for (Item& item : canvas.items) {
            // A capture still waiting to be written, named in the copy's
            // record by whatever the copy calls it.
            if (const auto pending = pendingPictures_.find(item.id); pending != pendingPictures_.end()) {
                if (Layer* picture = item.ImageLayer()) {
                    const std::optional<std::string> filename = copy.SaveImage(
                        item.id, pending->second.pixelsRGBA.data(), pending->second.width, pending->second.height);
                    if (filename) {
                        picture->imageFile = *filename;
                    } else {
                        wroteEverything = false;
                    }
                }
            }
            // Painted pixels held in memory, dirty or not: the copy has no
            // other source for them.
            for (size_t index = 0; index < item.layers.size(); ++index) {
                Layer& layer = item.layers[index];
                if (!layer.HasPaintedPixels()) {
                    continue;
                }
                const std::optional<std::string> filename =
                    copy.SaveLayerImage(item.id, index, layer.painted->PixelsRGBA().data(), layer.painted->Width(),
                                        layer.painted->Height());
                if (filename) {
                    layer.imageFile = *filename;
                } else {
                    wroteEverything = false;
                }
            }
        }
    }
    return copy.Save(snapshot) && wroteEverything;
}

bool Session::HasUnsavedChanges() const {
    return library_.manager.Generation() != library_.lastSavedGeneration || !pendingPictures_.empty();
}

void Session::SyncTexturesToCurrentCanvas() {
    if (!Store() || !window_) {
        return;
    }
    // Before anything is dropped, not after: the sync below throws away
    // every non-current canvas's painted pixels, and pixels that were never
    // written are the only copy there is. Waiting for the debounced
    // autosave was not good enough - switching canvas calls MarkChanged
    // itself, which pushes that save *further* away at exactly the moment
    // the pixels are discarded. Cheap in the ordinary case: only layers
    // actually painted on since their last write are re-encoded.
    SavePaintedLayers(library_);

    Manager().SyncShotTexturesToCanvas(
        Manager().CurrentCanvasId(),
        [this](const Item& item, Layer& layer) -> uint64_t {
            // Still holding its pixels - because they weren't safely on
            // disk to drop (see the release path below) - so they, not the
            // file, are what this layer actually is.
            if (layer.HasPaintedPixels()) {
                return window_->CreateTextureFromPixels(layer.painted->PixelsRGBA().data(),
                                                         layer.painted->Width(), layer.painted->Height());
            }
            const std::optional<persistence::DecodedImage> decoded = Store()->LoadImage(item.id, layer.imageFile);
            if (!decoded.has_value()) {
                return 0;
            }
            // A painted layer needs the pixels in memory as well as on the
            // GPU: a brush composites against what is already there, and a
            // texture is write-only from this side. An Image layer needs no
            // CPU copy, which is most of them and all of the big ones.
            if (layer.kind == LayerKind::Painted) {
                layer.painted = std::make_shared<PaintedImage>(
                    PaintedImage::FromPixels(decoded->width, decoded->height, decoded->pixelsRGBA));
            }
            return window_->CreateTextureFromPixels(decoded->pixelsRGBA.data(), decoded->width, decoded->height);
        },
        [this](Layer& layer) {
            // Offered for release because it is resident - a texture, or
            // pixels, or both (see SyncShotTexturesToCanvas) - so either
            // half may be absent.
            if (layer.textureHandle != 0) {
                window_->ReleaseTexture(layer.textureHandle);
            }
            // The pixels go too - holding a canvas's worth for every canvas
            // is the unbounded growth per-canvas syncing exists to avoid -
            // but only once they are safely on disk. Still dirty here means
            // the write above failed, and these are the only copy: keeping
            // them costs memory, dropping them costs the drawing.
            if (!layer.paintedDirty) {
                layer.painted.reset();
            }
        });
    shotTextureCanvasId_ = Manager().CurrentCanvasId();
}

void Session::EnsureTexturesForCurrentCanvas() {
    if (shotTextureCanvasId_ != Manager().CurrentCanvasId()) {
        SyncTexturesToCurrentCanvas();
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
    // in the meantime keep counting towards the quiet period as usual, and
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
    // everything it covers is on disk, and a painted layer's pixels - or a
    // capture's still waiting to be written - are covered by it as much as
    // the record that names them. The metadata is written even when a
    // picture wasn't - what did land is worth having, and the whole thing
    // is retried until all of it has.
    const uint64_t generation = instance.manager.Generation();
    const bool pixelsSaved = SavePaintedLayers(instance);
    const bool picturesSaved = SavePendingPictures(instance);
    const bool metadataSaved = instance.store->Save(instance.manager.ExportSnapshot());
    const bool saved = pixelsSaved && picturesSaved && metadataSaved;
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

// ================= Deleting and restoring =================

bool Session::Delete(uint64_t id) {
    if (!Manager().MarkDeleted(id, static_cast<int64_t>(std::time(nullptr)))) {
        return false;
    }
    // Hidden now - and what leaves the screen gives its pictures back in the
    // same frame, as anything leaving it does. The gated form would see the
    // same canvas current and do nothing.
    SyncTexturesToCurrentCanvas();
    return true;
}

bool Session::Restore(uint64_t id) {
    if (!Manager().Restore(id)) {
        return false;
    }
    SyncTexturesToCurrentCanvas();
    return true;
}

Session::Removal Session::DeletePermanently(uint64_t id) {
    CanvasManager& manager = Manager();
    // What goes with it: the textures of every snippet under it, and the
    // history of every canvas - or, for a lone snippet, its own entries on
    // its canvas's history, and nothing else of that canvas's.
    const bool isFolder = manager.FindFolder(id) != nullptr;
    bool found = isFolder;
    for (Canvas& canvas : manager.CanvasesMutable()) {
        const bool wholeCanvas = canvas.id == id || (isFolder && canvas.folderId == id);
        if (wholeCanvas) {
            found = true;
            DropHistoryOfCanvas(canvas.id);
        }
        for (Item& item : canvas.items) {
            if (!wholeCanvas && item.id != id) {
                continue;
            }
            found = true;
            if (!wholeCanvas) {
                ForgetHistoryOfItem(canvas.id, item.id);
            }
            for (Layer& layer : item.layers) {
                if (layer.textureHandle != 0 && window_) {
                    window_->ReleaseTexture(layer.textureHandle);
                }
                layer.textureHandle = 0;
            }
        }
    }
    if (!found || !manager.Erase(id)) {
        return Removal::NotFound;
    }
    // Off the disk now, rather than left for the next save - which would
    // take a directory the model no longer holds for something gone missing,
    // and set it aside (see LibraryStore::Save). A false from the store is
    // one of two things: nothing was ever saved for it, which is fine, or
    // something in its directory could not be removed, which the store
    // keeps a note of and the caller is told about.
    Removal removal = Removal::Removed;
    if (Store() && !Store()->Remove(id) && Store()->HasPendingRemoval(id)) {
        removal = Removal::FilesRemain;
    }
    SyncTexturesToCurrentCanvas();
    return removal;
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
    if (frozenScreenTexture_ != 0) {
        // Kept for CaptureShotItem to crop out of. Moved rather than copied:
        // this is a full screen's worth of pixels and nothing else wants
        // them.
        frozenScreenPixels_ = std::move(capture.pixelsRGBA);
    }
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
    result.textureHandle = window_->CreateTextureFromPixels(result.pixelsRGBA.data(), width, height);
    if (result.textureHandle == 0) {
        return std::nullopt;  // let the caller fall back rather than return a blank
    }
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
    Layer* picture = item.ImageLayer();
    if (!picture) {
        return;  // nothing to capture into - see Item::layers
    }
    // Placeholder gradient seed, used as a fallback below (and always
    // needed if a later resize or a canvas-to-canvas copy loses the real
    // capture - see Layer::textureHandle's own doc comment).
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
        if (const std::optional<std::string> filename =
                Store()->SaveImage(item.id, result.pixelsRGBA.data(), result.width, result.height)) {
            picture->imageFile = *filename;
            pendingPictures_.erase(item.id);
        } else {
            pendingPictures_[item.id] = PendingPicture{std::move(result.pixelsRGBA), result.width, result.height};
        }
    }
}

void Session::CloneShotImageForCopy(ItemId sourceId, ItemId copyId) {
    if (!Store()) {
        return;
    }
    const Item* source = Manager().FindItemAnywhere(sourceId);
    const Layer* sourcePicture = source ? source->ImageLayer() : nullptr;
    if (!sourcePicture || sourcePicture->imageFile.empty()) {
        return;  // a Drawing item, or a Shot item that never captured anything real
    }
    const std::optional<persistence::DecodedImage> decoded = Store()->LoadImage(sourceId, sourcePicture->imageFile);
    if (!decoded.has_value()) {
        return;
    }
    Item* copy = Manager().FindItemAnywhere(copyId);
    Layer* copyPicture = copy ? copy->ImageLayer() : nullptr;
    if (!copyPicture) {
        return;
    }
    // Re-saved under `copyId`'s own filename - a fresh file on disk, not a
    // second reference to the source's - same single-owner reasoning as
    // CanvasManager clearing these on copy in the first place. A write
    // that fails is kept for the next save, as a capture's is.
    if (window_) {
        copyPicture->textureHandle =
            window_->CreateTextureFromPixels(decoded->pixelsRGBA.data(), decoded->width, decoded->height);
    }
    if (const std::optional<std::string> filename = Store()->SaveImage(
            copyId, decoded->pixelsRGBA.data(), decoded->width, decoded->height)) {
        copyPicture->imageFile = *filename;
        pendingPictures_.erase(copyId);
    } else {
        pendingPictures_[copyId] = PendingPicture{std::move(decoded->pixelsRGBA), decoded->width, decoded->height};
    }
    // No MarkChanged() call needed - same reasoning as CaptureShotItem's
    // own: this runs synchronously right after the copy itself
    // (CanvasManager::DuplicateItem/MoveOrCopyItemToCanvas), which already
    // bumped the generation counter moments earlier in the same call stack.
}

}  // namespace sz::core
