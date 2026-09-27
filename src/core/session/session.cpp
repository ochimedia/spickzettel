#include "core/session/session.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>
#include <string>
#include <utility>
#include <vector>

#include "core/canvas/item_geometry.h"

namespace sz::core {

// ================= Writing =================

bool Session::Land(const Checkpoint& before) {
    std::vector<Captured> captured = std::exchange(captured_, {});
    std::vector<std::pair<ItemId, ItemId>> copies = std::exchange(pictureCopies_, {});
    if (!Store()) {
        return true;
    }
    persistence::LibraryStore::PictureWrites pictures;
    for (const Captured& picture : captured) {
        pictures.captured.push_back({picture.item, picture.pixelsRGBA.data(), picture.width, picture.height});
    }
    pictures.copies = std::move(copies);
    if (Store()->Write(Model().View(), Model().ChangesSince(before), pictures)) {
        lastWriteFailed_ = false;
        return true;
    }
    // Not written, so not made: the model goes back to what the file holds.
    // A texture the command made - a capture's - is asked for by no one
    // from here, and goes with the next frame (see TextureCache).
    lastWriteFailed_ = true;
    ++failedWrites_;
    migrations_.clear();
    Model().RollBack(before);
    liveLayer_.Clear();
    return false;
}

bool Session::WriteWholeLibrary() {
    if (!Store()) {
        return true;
    }
    lastWriteFailed_ = !Store()->Save(Model().View());
    failedWrites_ += lastWriteFailed_ ? 1 : 0;
    return !lastWriteFailed_;
}

bool Session::Commit(const Checkpoint& before, CanvasId canvas, history::Step step) {
    if (!Land(before)) {
        return false;
    }
    for (const auto& [item, to] : std::exchange(migrations_, {})) {
        history_.Migrate(item, to);
    }
    if (step.changes.empty()) {
        return true;
    }
    history_.Record(canvas, std::move(step));
    return true;
}

size_t Session::EraseForGood(const std::vector<uint64_t>& ids) {
    // Every snippet under what goes, copied, so that a write that fails can
    // put it all back.
    std::vector<ItemId> under;
    std::vector<CanvasId> canvases;
    for (const uint64_t id : ids) {
        const bool isFolder = Model().FindFolder(id) != nullptr;
        for (const Canvas& canvas : Model().Canvases()) {
            const bool whole = canvas.id == id || (isFolder && canvas.folderId == id);
            if (whole) {
                canvases.push_back(canvas.id);
            }
            for (const Item& item : canvas.items) {
                if (whole || item.id == id) {
                    under.push_back(item.id);
                }
            }
        }
    }
    const Checkpoint before = Before(under);
    size_t erased = 0;
    for (const uint64_t id : ids) {
        erased += Model().Erase(id) ? 1 : 0;
    }
    if (erased == 0 || !Land(before)) {
        return 0;
    }
    // Gone from the file: now every change about any of it on any canvas's
    // history, with the stacks of every canvas that went and every move from
    // or to one (see history::History::ForgetCanvas).
    for (const Item& item : before.items) {
        history_.ForgetItem(item.id);
    }
    for (const CanvasId canvas : canvases) {
        history_.ForgetCanvas(canvas);
    }
    return erased;
}

// ================= Changes that are not undone =================

void Session::SwitchToCanvas(CanvasId id) {
    if (id == Model().CurrentCanvasId()) {
        return;
    }
    if (!EndOpenGesture()) {
        return;
    }
    const Checkpoint before = Before({});
    Model().SwitchToCanvas(id);
    if (Model().CurrentCanvasId() == id) {
        liveLayer_.Clear();
    }
    Land(before);
}

void Session::SwitchToFolder(FolderId id) {
    if (!EndOpenGesture()) {
        return;
    }
    const Checkpoint before = Before({});
    Model().SwitchToFolder(id);
    Land(before);
}

CanvasId Session::AddCanvas(std::string name) {
    if (!EndOpenGesture()) {
        return 0;
    }
    const Checkpoint before = Before({});
    const CanvasId current = Model().CurrentCanvasId();
    const CanvasId id = Model().AddCanvas(std::move(name));
    // The first canvas of an empty library is current as it is made.
    if (Model().CurrentCanvasId() != current) {
        liveLayer_.Clear();
    }
    return Land(before) ? id : 0;
}

FolderId Session::AddFolder(std::string name) {
    if (!EndOpenGesture()) {
        return 0;
    }
    const Checkpoint before = Before({});
    const FolderId id = Model().AddFolder(std::move(name));
    return Land(before) ? id : 0;
}

void Session::RenameFolder(FolderId id, std::string name) {
    if (!EndOpenGesture()) {
        return;
    }
    const Checkpoint before = Before({});
    Model().RenameFolder(id, std::move(name));
    Land(before);
}

void Session::RenameCanvas(CanvasId id, std::string name) {
    if (!EndOpenGesture()) {
        return;
    }
    const Checkpoint before = Before({});
    Model().RenameCanvas(id, std::move(name));
    Land(before);
}

void Session::ReorderFolder(FolderId id, size_t newIndex) {
    if (!EndOpenGesture()) {
        return;
    }
    const Checkpoint before = Before({});
    Model().ReorderFolder(id, newIndex);
    Land(before);
}

void Session::ReorderCanvas(CanvasId id, size_t newIndex) {
    if (!EndOpenGesture()) {
        return;
    }
    const Checkpoint before = Before({});
    Model().ReorderCanvas(id, newIndex);
    Land(before);
}

void Session::MoveCanvasToFolder(CanvasId canvasId, FolderId folderId) {
    if (!EndOpenGesture()) {
        return;
    }
    const Checkpoint before = Before({});
    Model().MoveCanvasToFolder(canvasId, folderId);
    Land(before);
}

void Session::SetPinned(const std::vector<ItemId>& ids, bool pinned) {
    if (!EndOpenGesture()) {
        return;
    }
    const Checkpoint before = Before(ids);
    for (const ItemId id : ids) {
        if (Item* item = Model().FindItemAnywhere(id); item != nullptr && item->pinned != pinned) {
            item->pinned = pinned;
            Model().MarkChanged();
        }
    }
    Land(before);
}

void Session::SetMinimized(const std::vector<ItemId>& ids, bool minimized) {
    if (!EndOpenGesture()) {
        return;
    }
    const Checkpoint before = Before(ids);
    for (const ItemId id : ids) {
        if (Item* item = Model().FindItemAnywhere(id); item != nullptr && item->minimized != minimized) {
            item->minimized = minimized;
            Model().MarkChanged();
        }
    }
    Land(before);
}

void Session::BringItemsToFront(const std::vector<ItemId>& ids) {
    if (!EndOpenGesture()) {
        return;
    }
    const Checkpoint before = Before({});
    Model().BringItemsToFront(ids);
    Land(before);
}

void Session::MoveItemLayer(ItemId id, int direction) {
    if (!EndOpenGesture()) {
        return;
    }
    const Checkpoint before = Before({});
    Model().MoveItemLayer(id, direction);
    Land(before);
}

void Session::SyncItemsToDisplaySize(float width, float height) { Model().SyncItemsToDisplaySize(width, height); }

// ================= Deleting and restoring =================

void Session::ImportLibrary(CanvasManagerSnapshot snapshot) {
    manager_.ImportSnapshot(std::move(snapshot));
    EraseForGood(Model().MarkedSnippets());
}

bool Session::Delete(uint64_t id) {
    if (!EndOpenGesture()) {
        return false;
    }
    if (Model().FindFolder(id) == nullptr && Model().FindCanvas(id) == nullptr) {
        return false;
    }
    const Checkpoint before = Before({});
    return Model().MarkDeleted(id, static_cast<int64_t>(std::time(nullptr))) && Land(before);
}

bool Session::Restore(uint64_t id) {
    if (!EndOpenGesture()) {
        return false;
    }
    if (Model().FindFolder(id) == nullptr && Model().FindCanvas(id) == nullptr) {
        return false;
    }
    const Checkpoint before = Before({});
    return Model().Restore(id) && Land(before);
}

bool Session::DeletePermanently(uint64_t id) {
    if (!EndOpenGesture()) {
        return false;
    }
    return EraseForGood({id}) != 0;
}

bool Session::DeleteMarkedCanvasesPermanently(FolderId folderId) {
    if (!EndOpenGesture()) {
        return false;
    }
    const std::vector<CanvasId> marked = Model().MarkedCanvasesIn(folderId);
    return !marked.empty() && EraseForGood(marked) != 0;
}

size_t Session::EraseDeletedBefore(int64_t cutoff) { return EraseForGood(Model().MarkedBefore(cutoff)); }

// ================= Capturing the screen =================

void Session::FreezeScreen(const platform::DisplayInfo& display) {
    ReleaseFrozenScreen();
    if (!window_ || display.width <= 0 || display.height <= 0) {
        return;
    }
    // The whole display the overlay is on. The window is sized to it, and
    // the capture excludes the overlay's own content (see
    // IOverlayWindow::CaptureRegion), so what comes back is the application
    // underneath and nothing of ours.
    platform::CaptureResult capture = window_->CaptureRegion(
        platform::Rect{0.0f, 0.0f, static_cast<float>(display.width), static_cast<float>(display.height)});
    // No pixels is the ordinary "couldn't" answer - a backend without
    // capture, or a display the OS won't hand over - and it needs no
    // special case: with nothing frozen the overlay just stays transparent,
    // exactly as it behaves with the setting off.
    //
    // Kept for CaptureShotItem to crop out of, and for the texture to be
    // made from - again, should the device be replaced while it is held.
    // Moved rather than copied: this is a full screen's worth of pixels and
    // nothing else wants them.
    frozenScreenWidth_ = capture.width;
    frozenScreenHeight_ = capture.height;
    frozenScreenPixels_ = std::move(capture.pixelsRGBA);
}

uint64_t Session::FrozenScreenTexture() {
    const size_t expected = static_cast<size_t>(frozenScreenWidth_) * static_cast<size_t>(frozenScreenHeight_) * 4u;
    if (frozenScreenPixels_.empty() || frozenScreenPixels_.size() < expected) {
        return 0;
    }
    return textures_.Get(TextureKey{TextureKey::Kind::FrozenScreen, 0}, 0,
                         TexturePixels{frozenScreenPixels_.data(), frozenScreenWidth_, frozenScreenHeight_});
}

// A sub-rectangle of the frozen screen, returned in the same shape a live
// capture would come back in - so the caller can treat the two
// identically. nullopt means "nothing frozen, go and capture", which
// covers the setting being off, the freeze having failed, and a backend
// that can't capture at all.
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
    return result;
}

void Session::ReleaseFrozenScreen() {
    textures_.Drop(TextureKey{TextureKey::Kind::FrozenScreen, 0});
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
    // Placeholder gradient seed, what is drawn while there are no pixels to
    // show - a capture that failed, or a picture that cannot be read.
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
        cropped.has_value()
            ? std::move(*cropped)
            : window_->CaptureRegion(platform::Rect{item.rect.x, item.rect.y, item.rect.w, item.rect.h});
    if (result.pixelsRGBA.empty()) {
        return;
    }
    // Its texture from the pixels at hand, rather than read back from the
    // library by the first frame that draws it. An upload that fails - the
    // device lost, say - is tried again once the device is replaced, from
    // the library.
    textures_.Put(TextureKey{TextureKey::Kind::Picture, item.id},
                  TexturePixels{result.pixelsRGBA.data(), result.width, result.height});
    // Written with the snippet it belongs to, in the same transaction (see
    // Land): a write that fails takes the snippet back out with its
    // picture, rather than leaving one on screen that looks captured and is
    // not in the library.
    if (Store()) {
        captured_.push_back(Captured{item.id, std::move(result.pixelsRGBA), result.width, result.height});
        picture->stored = true;
    }
}

bool Session::ClonePicturesForCopy(ItemId sourceId, ItemId copyId) {
    if (!Store()) {
        return true;
    }
    const Item* source = Model().FindItemAnywhere(sourceId);
    Item* copy = Model().FindItemAnywhere(copyId);
    if (!source || !copy || !source->picture.stored) {
        return true;  // nothing to copy
    }
    // Copied as stored, without decoding it, in the same write as the copy.
    // Its texture is made from it the first time it is drawn.
    if (!Store()->HasImage(sourceId)) {
        return false;  // said to be stored, and it is not: the copy lacks it
    }
    pictureCopies_.emplace_back(sourceId, copyId);
    copy->picture.stored = true;
    return true;
}

}  // namespace sz::core
