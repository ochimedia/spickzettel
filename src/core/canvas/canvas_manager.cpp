#include "core/canvas/canvas_manager.h"

#include <algorithm>

#include "core/canvas/item_geometry.h"
#include "core/drawing/stroke_clip.h"
#include "core/util/timestamp_name.h"
#include "core/util/uid.h"

namespace sz::core {

namespace {

// What a copied item's picture has to give up so the copy owns its own
// resources. A texture handle has single-owner lifetime (see picture.h) and
// must not be duplicated. The stored pixels are the source's, so the copy
// starts without them and the caller gives it its own (see
// Session::ClonePicturesForCopy); this class has no store.
void DetachPictureForCopy(Item& copied) {
    copied.picture.textureHandle = 0;
    copied.picture.stored = false;
}

}  // namespace

CanvasManager::CanvasManager(std::string initialCanvasName) {
    // Named for the moment, like every folder and canvas nobody has named
    // - see TimestampName.
    Folder folder;
    folder.id = NewId();
    folder.name = TimestampName();
    folders_.push_back(folder);
    currentFolderId_ = folder.id;
    AddCanvas(initialCanvasName.empty() ? TimestampName() : std::move(initialCanvasName));
    // AddCanvas() above bumps this, same as it would for a genuine
    // user-driven canvas creation - but constructing a fresh manager
    // isn't itself a user change, so there's nothing to autosave yet.
    // See Generation()'s own doc comment.
    generation_ = 0;
}

void CanvasManager::SwitchToNextAvailableCanvas(std::optional<FolderId> preferredFolderId) {
    if (preferredFolderId.has_value()) {
        const auto sameFolder = std::find_if(canvases_.begin(), canvases_.end(),
                                              [&](const Canvas& c) { return c.folderId == *preferredFolderId; });
        if (sameFolder != canvases_.end()) {
            currentCanvasId_ = sameFolder->id;
            currentFolderId_ = sameFolder->folderId;
            return;
        }
    }
    if (canvases_.empty()) {
        // Nothing left to switch to - a legal state now (see the class
        // comment). currentFolderId_ is deliberately left alone: the
        // folder may well still exist and still be the one being browsed,
        // it just has nothing in it. DeleteFolder fixes it up itself when
        // the folder is what went away.
        currentCanvasId_ = 0;
        return;
    }
    currentCanvasId_ = canvases_.front().id;
    currentFolderId_ = canvases_.front().folderId;
}

CanvasId CanvasManager::AddCanvas(std::string name) {
    // No folder to put this in - every one deleted (that's allowed - see the
    // class comment), which leaves nothing browsed. Mint one rather than hand
    // back a canvas whose folderId points at nothing.
    if (FindFolder(currentFolderId_) == nullptr) {
        AddFolder(TimestampName());
    }
    Canvas canvas;
    canvas.id = NewId();
    canvas.name = std::move(name);
    canvas.folderId = currentFolderId_;
    const CanvasId newId = canvas.id;
    // After the last canvas already in this folder, so a new one lands at
    // the end of its own folder and every other folder's canvases stay
    // exactly where they were. A folder with nothing in it yet has no such
    // canvas to follow, so its first one goes at the end of the list.
    auto insertAt = canvases_.end();
    for (auto it = canvases_.begin(); it != canvases_.end(); ++it) {
        if (it->folderId == canvas.folderId) {
            insertAt = it + 1;
        }
    }
    canvases_.insert(insertAt, std::move(canvas));
    // Not "size() == 1": with an empty library reachable, a canvas can be
    // added back while currentCanvasId_ is still 0, and the count alone
    // no longer tells the two apart.
    if (currentCanvasId_ == 0) {
        currentCanvasId_ = newId;
    }
    MarkChanged();
    return newId;
}

void CanvasManager::SwitchToCanvas(CanvasId id) {
    if (id == currentCanvasId_) {
        return;
    }
    const auto it = std::find_if(canvases_.begin(), canvases_.end(),
                                  [id](const Canvas& c) { return c.id == id; });
    if (it == canvases_.end() || IsDeleted(*it)) {
        return;
    }
    currentCanvasId_ = id;
    currentFolderId_ = it->folderId;
    MarkChanged();
}

void CanvasManager::DeleteCanvas(CanvasId id) {
    const auto it = std::find_if(canvases_.begin(), canvases_.end(),
                                  [id](const Canvas& c) { return c.id == id; });
    if (it == canvases_.end()) {
        return;
    }
    const bool wasCurrent = (id == currentCanvasId_);
    const FolderId folderId = it->folderId;
    // Where it sat among its own folder's canvases, counted before the
    // erase - which is what "the one before it" below is measured against.
    size_t indexInFolder = 0;
    for (auto scan = canvases_.begin(); scan != it; ++scan) {
        if (scan->folderId == folderId) {
            ++indexInFolder;
        }
    }
    canvases_.erase(it);
    if (wasCurrent) {
        // Stay in the folder the deleted canvas lived in, on its
        // neighbor: the one before it, or the folder's first if it was
        // the first, or nothing at all if that folder is now empty - see
        // the header for why "nothing at all" rather than some other
        // folder's canvas, and who fills the gap.
        std::vector<CanvasId> siblings;
        for (const Canvas& canvas : canvases_) {
            if (canvas.folderId == folderId) {
                siblings.push_back(canvas.id);
            }
        }
        currentFolderId_ = folderId;
        if (siblings.empty()) {
            currentCanvasId_ = 0;
        } else {
            const size_t target = indexInFolder > 0 ? indexInFolder - 1 : 0;
            currentCanvasId_ = siblings[std::min(target, siblings.size() - 1)];
        }
    }
    MarkChanged();
}

void CanvasManager::ReorderCanvas(CanvasId id, size_t newIndex) {
    const auto it = std::find_if(canvases_.begin(), canvases_.end(),
                                  [id](const Canvas& c) { return c.id == id; });
    if (it == canvases_.end()) {
        return;
    }
    const FolderId folderId = it->folderId;
    Canvas moved = std::move(*it);
    canvases_.erase(it);

    // Reorder within the same-folder subsequence only - other folders'
    // canvases keep their relative position in `canvases_` undisturbed.
    std::vector<size_t> folderPositions;
    for (size_t i = 0; i < canvases_.size(); ++i) {
        if (canvases_[i].folderId == folderId) {
            folderPositions.push_back(i);
        }
    }
    newIndex = std::min(newIndex, folderPositions.size());
    const size_t insertPos = newIndex < folderPositions.size() ? folderPositions[newIndex]
                              : folderPositions.empty()         ? canvases_.size()
                                                                 : folderPositions.back() + 1;
    canvases_.insert(canvases_.begin() + static_cast<std::ptrdiff_t>(insertPos), std::move(moved));
    MarkChanged();
}

FolderId CanvasManager::AddFolder(std::string name) {
    Folder folder;
    folder.id = NewId();
    folder.name = std::move(name);
    // At the end, like a new canvas within its folder: a list you keep
    // adding to reads in the order things were made, and the sidebar's own
    // "New folder" button sits at the bottom, which is where the eye
    // already is when one appears.
    folders_.push_back(folder);
    currentFolderId_ = folder.id;
    MarkChanged();
    return folder.id;
}

void CanvasManager::RenameFolder(FolderId id, std::string name) {
    if (name.empty()) {
        return;
    }
    const auto it = std::find_if(folders_.begin(), folders_.end(), [id](const Folder& f) { return f.id == id; });
    if (it == folders_.end()) {
        return;
    }
    it->name = std::move(name);
    MarkChanged();
}

void CanvasManager::RenameCanvas(CanvasId id, std::string name) {
    if (name.empty()) {
        return;
    }
    const auto it = std::find_if(canvases_.begin(), canvases_.end(), [id](const Canvas& c) { return c.id == id; });
    if (it == canvases_.end()) {
        return;
    }
    it->name = std::move(name);
    MarkChanged();
}

void CanvasManager::SwitchToFolder(FolderId id) {
    if (id == currentFolderId_) {
        return;
    }
    const Folder* folder = FindFolder(id);
    if (folder == nullptr || IsDeleted(*folder)) {
        return;
    }
    currentFolderId_ = id;
    MarkChanged();
}

void CanvasManager::DeleteFolder(FolderId id) {
    const auto it = std::find_if(folders_.begin(), folders_.end(), [id](const Folder& f) { return f.id == id; });
    if (it == folders_.end()) {
        return;
    }
    const bool wasCurrentFolder = (id == currentFolderId_);
    const auto curCanvasIt = std::find_if(canvases_.begin(), canvases_.end(),
                                           [this](const Canvas& c) { return c.id == currentCanvasId_; });
    const bool currentCanvasInDeletedFolder = curCanvasIt != canvases_.end() && curCanvasIt->folderId == id;

    // Erases every canvas in this folder, and every item on those, for
    // good - deleting a folder is immediate and final, same as deleting a
    // canvas or item directly (see the class comment).
    canvases_.erase(std::remove_if(canvases_.begin(), canvases_.end(), [id](const Canvas& c) { return c.folderId == id; }),
                     canvases_.end());
    folders_.erase(it);

    if (currentCanvasInDeletedFolder) {
        // Re-homes currentFolderId_ onto the new current canvas's folder
        // as a side effect - unless there are no canvases left at all, in
        // which case it deliberately leaves it alone (see its own
        // comment) and the check below is what catches it.
        SwitchToNextAvailableCanvas(std::nullopt);
    }
    // Asked directly rather than as an else-branch: two cases leave the
    // browsed folder dangling (it was the browsed folder; or it was both
    // browsed and the current canvas's with no other canvas to re-home
    // onto), and neither lines up with a branch of the above.
    if (wasCurrentFolder &&
        !std::any_of(folders_.begin(), folders_.end(),
                      [this](const Folder& f) { return f.id == currentFolderId_; })) {
        currentFolderId_ = folders_.empty() ? 0 : folders_.front().id;
    }
    MarkChanged();
}

void CanvasManager::ReorderFolder(FolderId id, size_t newIndex) {
    const auto it = std::find_if(folders_.begin(), folders_.end(), [id](const Folder& f) { return f.id == id; });
    if (it == folders_.end()) {
        return;
    }
    Folder moved = std::move(*it);
    folders_.erase(it);
    newIndex = std::min(newIndex, folders_.size());
    folders_.insert(folders_.begin() + static_cast<std::ptrdiff_t>(newIndex), std::move(moved));
    MarkChanged();
}

void CanvasManager::MoveCanvasToFolder(CanvasId canvasId, FolderId targetFolderId) {
    const auto it = std::find_if(canvases_.begin(), canvases_.end(),
                                  [canvasId](const Canvas& c) { return c.id == canvasId; });
    const Folder* target = FindFolder(targetFolderId);
    if (it == canvases_.end() || it->folderId == targetFolderId || target == nullptr || IsDeleted(*target)) {
        return;  // a canvas moved into a deleted folder would be deleted with it
    }
    // Folders are allowed to end up empty - moving a folder's only canvas
    // elsewhere is legal, not a no-op.
    it->folderId = targetFolderId;
    MarkChanged();
}

Canvas* CanvasManager::CurrentOrNull() {
    const auto it = std::find_if(canvases_.begin(), canvases_.end(),
                                  [this](const Canvas& c) { return c.id == currentCanvasId_; });
    return it == canvases_.end() ? nullptr : &*it;
}

const Canvas* CanvasManager::CurrentOrNull() const {
    const auto it = std::find_if(canvases_.begin(), canvases_.end(),
                                  [this](const Canvas& c) { return c.id == currentCanvasId_; });
    return it == canvases_.end() ? nullptr : &*it;
}

std::vector<uint64_t> CanvasManager::CaptureTextureHandlesForCanvas(CanvasId id) const {
    std::vector<uint64_t> handles;
    const auto it = std::find_if(canvases_.begin(), canvases_.end(), [id](const Canvas& c) { return c.id == id; });
    if (it == canvases_.end()) {
        return handles;
    }
    for (const Item& item : it->items) {
        if (item.picture.textureHandle != 0) {
            handles.push_back(item.picture.textureHandle);
        }
    }
    return handles;
}

ItemId CanvasManager::CreateItem(bool hasBackground, Rect rect, std::string name) {
    Item item;
    item.hasBackground = hasBackground;
    // Opaque by default for a Screenshot, none at all for a Drawing, whose
    // picture starts fully transparent and waits to be given something;
    // tintColorRGBA stays at its own in-class default (white) either way.
    item.picture.opacity = hasBackground ? 1.0f : 0.0f;
    item.name = std::move(name);
    item.rect = rect;
    return CreateItem(std::move(item));
}

ItemId CanvasManager::CreateItem(Item prototype) {
    Canvas* canvas = CurrentOrNull();
    if (!canvas) {
        return 0;  // nothing to create it on - see the class comment
    }
    Item item = std::move(prototype);
    item.id = NewId();
    item.deletedAt = 0;
    item.picture.showsPlaceholder = item.hasBackground;
    item.picture.stored = false;
    item.picture.textureHandle = 0;
    item.nativeW = item.rect.w;
    item.nativeH = item.rect.h;
    item.anchorRect = item.rect;
    item.anchorDisplayWidth = currentDisplayWidth_;
    item.anchorDisplayHeight = currentDisplayHeight_;
    canvas->items.push_back(std::move(item));
    MarkChanged();
    return canvas->items.back().id;
}

Item* CanvasManager::FindInCurrent(ItemId id) {
    Canvas* canvas = CurrentOrNull();
    if (!canvas) {
        return nullptr;
    }
    auto& items = canvas->items;
    const auto it = std::find_if(items.begin(), items.end(), [id](const Item& i) { return i.id == id; });
    return it == items.end() ? nullptr : &*it;
}

Item* CanvasManager::FindItemAnywhere(ItemId id) {
    for (Canvas& canvas : canvases_) {
        const auto it = std::find_if(canvas.items.begin(), canvas.items.end(),
                                      [id](const Item& i) { return i.id == id; });
        if (it != canvas.items.end()) {
            return &*it;
        }
    }
    return nullptr;
}

const Item* CanvasManager::FindItemAnywhere(ItemId id) const {
    return const_cast<CanvasManager*>(this)->FindItemAnywhere(id);
}

bool CanvasManager::CurrentCanvasHasPinnedItems() const {
    const Canvas* canvas = CurrentOrNull();
    return canvas != nullptr && std::any_of(canvas->items.begin(), canvas->items.end(), [&](const Item& item) {
               return item.pinned && !item.minimized && !IsDeleted(*canvas, item);
           });
}

void CanvasManager::DeleteItem(ItemId id) {
    Canvas* canvas = CurrentOrNull();
    if (!canvas) {
        return;
    }
    auto& items = canvas->items;
    const auto it = std::find_if(items.begin(), items.end(), [id](const Item& i) { return i.id == id; });
    if (it == items.end()) {
        return;
    }
    items.erase(it);
    MarkChanged();
}

void CanvasManager::DeleteItemFromCanvas(CanvasId canvasId, ItemId id) {
    const auto canvasIt =
        std::find_if(canvases_.begin(), canvases_.end(), [canvasId](const Canvas& c) { return c.id == canvasId; });
    if (canvasIt == canvases_.end()) {
        return;
    }
    auto& items = canvasIt->items;
    const auto itemIt = std::find_if(items.begin(), items.end(), [id](const Item& i) { return i.id == id; });
    if (itemIt == items.end()) {
        return;
    }
    items.erase(itemIt);
    MarkChanged();
}

std::optional<size_t> CanvasManager::IndexOfItemOnCanvas(const Canvas& canvas, ItemId id) const {
    for (size_t index = 0; index < canvas.items.size(); ++index) {
        if (canvas.items[index].id == id) {
            return index;
        }
    }
    return std::nullopt;
}

std::optional<size_t> CanvasManager::NearestOverlappingItem(const Canvas& canvas, size_t index, int direction) const {
    const Item& item = canvas.items[index];
    if (item.minimized || IsDeleted(canvas, item)) {
        return std::nullopt;  // nowhere on screen, so nothing to be in front of
    }
    const int step = direction > 0 ? 1 : -1;
    for (int at = static_cast<int>(index) + step; at >= 0 && at < static_cast<int>(canvas.items.size()); at += step) {
        const Item& other = canvas.items[static_cast<size_t>(at)];
        if (other.minimized || IsDeleted(canvas, other)) {
            continue;
        }
        if (RectsOverlap(item.rect, other.rect)) {
            return static_cast<size_t>(at);
        }
    }
    return std::nullopt;
}

void CanvasManager::MoveItemLayer(ItemId id, int direction) {
    Canvas* canvas = CurrentOrNull();
    if (!canvas) {
        return;
    }
    const std::optional<size_t> index = IndexOfItemOnCanvas(*canvas, id);
    if (!index.has_value()) {
        return;
    }
    const std::optional<size_t> past = NearestOverlappingItem(*canvas, *index, direction);
    if (!past.has_value()) {
        return;
    }
    auto& items = canvas->items;
    const auto at = items.begin() + static_cast<long>(*index);
    const auto other = items.begin() + static_cast<long>(*past);
    if (direction > 0) {
        // Everything from the item up to and including the one it passes
        // slides down one place, and the item lands where that one was.
        std::rotate(at, at + 1, other + 1);
    } else {
        std::rotate(other, at, at + 1);
    }
    MarkChanged();
}

bool CanvasManager::CanMoveItemLayer(ItemId id, int direction) const {
    const Canvas* canvas = CurrentOrNull();
    if (canvas == nullptr) {
        return false;
    }
    const std::optional<size_t> index = IndexOfItemOnCanvas(*canvas, id);
    return index.has_value() && NearestOverlappingItem(*canvas, *index, direction).has_value();
}

void CanvasManager::BringItemToFront(ItemId id) {
    Canvas* canvas = CurrentOrNull();
    if (!canvas) {
        return;
    }
    auto& items = canvas->items;
    const auto it = std::find_if(items.begin(), items.end(), [id](const Item& i) { return i.id == id; });
    if (it == items.end() || it + 1 == items.end()) {
        return;  // not on the current canvas, or already at the top
    }
    std::rotate(it, it + 1, items.end());
    MarkChanged();
}

void CanvasManager::BringItemsToFront(const std::vector<ItemId>& ids) {
    Canvas* canvas = CurrentOrNull();
    if (!canvas) {
        return;
    }
    const auto stays = [&ids](const Item& item) { return std::find(ids.begin(), ids.end(), item.id) == ids.end(); };
    auto& items = canvas->items;
    if (std::is_partitioned(items.begin(), items.end(), stays)) {
        return;  // none of them here, or on top already
    }
    std::stable_partition(items.begin(), items.end(), stays);
    MarkChanged();
}

void CanvasManager::ToggleFullscreen(ItemId id, float viewportW, float viewportH, bool stretch) {
    Item* item = FindInCurrent(id);
    if (!item) {
        return;
    }
    if (item->isFullscreen) {
        // Recomputed from the untouched anchor against the *current*
        // viewport, so it is right even if the display changed while this
        // item was fullscreen. Falls back to the raw anchor if the item
        // was never anchored against a known display size.
        if (item->anchorDisplayWidth > 0.0f && item->anchorDisplayHeight > 0.0f && viewportW > 0.0f &&
            viewportH > 0.0f) {
            const Rect restored =
                GrowRectToMinimumSize(RescaleRectForDisplaySize(item->anchorRect, item->anchorDisplayWidth,
                                                                 item->anchorDisplayHeight, viewportW, viewportH));
            item->rect = ClampRectToViewport(restored, viewportW, viewportH);
        } else {
            item->rect = item->anchorRect;
        }
        item->isFullscreen = false;
    } else {
        const bool useStretch =
            stretch || item->rect.w <= 0.0f || item->rect.h <= 0.0f || viewportW <= 0.0f || viewportH <= 0.0f;
        if (useStretch) {
            item->rect = Rect{0.0f, 0.0f, viewportW, viewportH};
        } else {
            // Fit the item's own aspect ratio into the viewport instead
            // of stretching it to match both dimensions exactly.
            item->rect = FitAspectRatioIntoViewport(item->rect.w / item->rect.h, viewportW, viewportH);
        }
        item->isFullscreen = true;
        item->isFullscreenStretch = useStretch;
    }
    MarkChanged();
}

void CanvasManager::SyncItemsToDisplaySize(float currentW, float currentH) {
    if (currentW <= 0.0f || currentH <= 0.0f) {
        return;  // nothing sane to sync against
    }
    currentDisplayWidth_ = currentW;
    currentDisplayHeight_ = currentH;

    bool changed = false;
    for (Canvas& canvas : canvases_) {
        for (Item& item : canvas.items) {
            if (item.isFullscreen) {
                // Recomputed fresh every call, not scaled from anything -
                // matching ToggleFullscreen's own entry logic exactly, so
                // a fit-mode fullscreen item keeps its aspect ratio intact
                // (not silently stretched) if the display size changes
                // while it's up. The anchor underneath is left alone (see
                // Item::anchorRect's own doc comment); the aspect ratio to
                // fit comes from it (the last non-fullscreen placement),
                // not from `rect` itself, since `rect` while fullscreen is
                // whatever this branch already set it to, not a usable
                // aspect-ratio source.
                Rect fullRect;
                if (item.isFullscreenStretch || item.anchorRect.w <= 0.0f || item.anchorRect.h <= 0.0f) {
                    fullRect = Rect{0.0f, 0.0f, currentW, currentH};
                } else {
                    fullRect = FitAspectRatioIntoViewport(item.anchorRect.w / item.anchorRect.h, currentW, currentH);
                }
                if (fullRect != item.rect) {
                    item.rect = fullRect;
                    changed = true;
                }
                continue;
            }
            if (item.anchorDisplayWidth <= 0.0f || item.anchorDisplayHeight <= 0.0f) {
                // Never anchored yet - a fresh item created before the
                // first display size was known. Nothing to reposition
                // against, so adopt the rect it already has as the anchor
                // and move on.
                item.anchorRect = item.rect;
                item.anchorDisplayWidth = currentW;
                item.anchorDisplayHeight = currentH;
                continue;
            }
            // Always recomputed fresh from the item's own fixed anchor,
            // never chained onto whatever `rect` already holds, and never
            // skipped just because `currentW`/`currentH` happens to match
            // the anchor's own reference size - `rect` can still be stale
            // from a *different* size synced to since the anchor was last
            // set (e.g. squeezed narrower, then widened back), and only
            // actually recomputing (not just comparing sizes) catches
            // that. RescaleRectForDisplaySize itself already short-
            // circuits to an exact identity when the sizes match, so this
            // costs nothing extra in the common steady-state case where
            // nothing has changed since the last call.
            Rect rescaled = RescaleRectForDisplaySize(item.anchorRect, item.anchorDisplayWidth,
                                                        item.anchorDisplayHeight, currentW, currentH);
            // Same floor an interactive resize enforces (see
            // ApplyResizeHandleDelta): a 4K library reopened on a small
            // laptop screen could otherwise shrink an item below anything
            // usable. Ratio-preserving, to match the uniform scale above.
            rescaled = ClampRectToViewport(GrowRectToMinimumSize(rescaled), currentW, currentH);
            if (rescaled != item.rect) {
                item.rect = rescaled;
                changed = true;
            }
        }
    }

    if (changed) {
        MarkChanged();
    }
}

void CanvasManager::CommitItemLayout(ItemId id) {
    if (currentDisplayWidth_ <= 0.0f || currentDisplayHeight_ <= 0.0f) {
        return;
    }
    Item* item = FindInCurrent(id);
    if (!item) {
        return;
    }
    item->anchorRect = item->rect;
    item->anchorDisplayWidth = currentDisplayWidth_;
    item->anchorDisplayHeight = currentDisplayHeight_;
}

void CanvasManager::ResetItemToNativeSize(ItemId id) {
    Item* item = FindInCurrent(id);
    if (!item || item->nativeW <= 0.0f || item->nativeH <= 0.0f) {
        return;
    }
    const float centerX = item->rect.x + item->rect.w * 0.5f;
    const float centerY = item->rect.y + item->rect.h * 0.5f;
    Rect restored{centerX - item->nativeW * 0.5f, centerY - item->nativeH * 0.5f, item->nativeW, item->nativeH};
    if (currentDisplayWidth_ > 0.0f && currentDisplayHeight_ > 0.0f) {
        restored = ClampRectToViewport(restored, currentDisplayWidth_, currentDisplayHeight_);
    }
    item->rect = restored;
    // A fixed native pixel size and a viewport-filling override are
    // contradictory - same reasoning as HandleItemGesture exiting fullscreen
    // before letting the user move/resize an item directly.
    item->isFullscreen = false;
    item->anchorRect = item->rect;
    item->anchorDisplayWidth = currentDisplayWidth_;
    item->anchorDisplayHeight = currentDisplayHeight_;
    MarkChanged();
}

std::optional<CanvasId> CanvasManager::CanvasHoldingItem(ItemId id) const {
    for (const Canvas& canvas : canvases_) {
        if (IndexOfItemOnCanvas(canvas, id).has_value()) {
            return canvas.id;
        }
    }
    return std::nullopt;
}

ItemId CanvasManager::PlaceItemOnCanvas(ItemId id, CanvasId targetCanvasId, bool copy,
                                        std::optional<size_t> atIndex) {
    const auto targetIt = std::find_if(canvases_.begin(), canvases_.end(),
                                        [targetCanvasId](const Canvas& c) { return c.id == targetCanvasId; });
    if (targetIt == canvases_.end() || IsDeleted(*targetIt)) {
        return 0;  // no such canvas, or one nothing can be seen on
    }
    Canvas* source = nullptr;
    size_t at = 0;
    for (Canvas& canvas : canvases_) {
        if (const std::optional<size_t> index = IndexOfItemOnCanvas(canvas, id)) {
            source = &canvas;
            at = *index;
            break;
        }
    }
    if (source == nullptr) {
        return 0;
    }
    if (copy) {
        Item copied = source->items[at];
        copied.id = NewId();
        // A new thing, not deleted whatever its source is: a copy that
        // arrived marked would be invisible where it landed, and listed
        // among the deleted with a stamp from before it existed.
        copied.deletedAt = 0;
        const ItemId newId = copied.id;
        DetachPictureForCopy(copied);
        // Read off `source` before this, which may be the very vector
        // being pushed to.
        targetIt->items.push_back(std::move(copied));
        MarkChanged();
        return newId;
    }
    if (source->id == targetCanvasId) {
        return id;  // already there: a move onto its own canvas moves nothing
    }
    const size_t to = std::min(atIndex.value_or(targetIt->items.size()), targetIt->items.size());
    targetIt->items.insert(targetIt->items.begin() + static_cast<long>(to), std::move(source->items[at]));
    source->items.erase(source->items.begin() + static_cast<long>(at));
    MarkChanged();
    return id;
}

ItemId CanvasManager::MoveOrCopyItemToCanvas(ItemId id, CanvasId targetCanvasId, bool copy) {
    if (targetCanvasId == currentCanvasId_ || FindInCurrent(id) == nullptr) {
        return 0;  // from the current canvas, to another one: see the header
    }
    const ItemId placed = PlaceItemOnCanvas(id, targetCanvasId, copy);
    // A move reports nothing: the caller already knows `id`, unchanged.
    return copy ? placed : 0;
}

ItemId CanvasManager::DuplicateItem(ItemId id) {
    if (FindInCurrent(id) == nullptr) {
        return 0;
    }
    return PlaceItemOnCanvas(id, currentCanvasId_, /*copy=*/true);
}

namespace {
// The shared body of EraseAt and EraseRectAt: replaces each stroke `clip`
// touches with the fragments it returns, in place, and reports what became
// of each - see EraseAt's own doc comment for the contract.
template <typename Clip>
std::vector<size_t> ClipStrokesInPlace(Item& item, const Clip& clip) {
    std::vector<size_t> outcome;
    outcome.reserve(item.strokes.size());
    std::vector<Stroke> kept;
    kept.reserve(item.strokes.size());
    for (Stroke& stroke : item.strokes) {
        std::optional<std::vector<Stroke>> clipped = clip(stroke);
        if (!clipped.has_value()) {
            outcome.push_back(CanvasManager::kStrokeUntouched);
            kept.push_back(std::move(stroke));
            continue;
        }
        outcome.push_back(clipped->size());
        for (Stroke& fragment : *clipped) {
            kept.push_back(std::move(fragment));
        }
    }
    item.strokes = std::move(kept);
    return outcome;
}
}  // namespace

std::vector<size_t> CanvasManager::EraseAt(ItemId id, float screenX, float screenY, float radiusScreenPx) {
    // Marked changed whether or not anything came away: simpler than
    // threading a \"did this actually change anything\" result out just to
    // decide whether to mark, and a spurious bump costs nothing (see
    // Generation()'s own doc comment: it only resets the autosave's
    // debounce timer).
    MarkChanged();
    Item* item = FindInCurrent(id);
    if (!item) {
        return {};
    }
    const NativePoint native = ScreenToNative(*item, screenX, screenY);
    const float nativeRadius = radiusScreenPx * native.scale;
    return ClipStrokesInPlace(*item, [&](const Stroke& stroke) {
        return ClipStrokeOutsideCircle(stroke, StrokePoint{native.x, native.y}, nativeRadius);
    });
}

std::vector<size_t> CanvasManager::EraseRectAt(ItemId id, float minX, float minY, float maxX, float maxY) {
    // See EraseAt on why this is unconditional.
    MarkChanged();
    Item* item = FindInCurrent(id);
    if (!item) {
        return {};
    }
    const NativePoint nativeMin = ScreenToNative(*item, minX, minY);
    const NativePoint nativeMax = ScreenToNative(*item, maxX, maxY);
    return ClipStrokesInPlace(*item, [&](const Stroke& stroke) {
        return ClipStrokeOutsideRect(stroke, nativeMin.x, nativeMin.y, nativeMax.x, nativeMax.y);
    });
}

Stroke CanvasManager::BakeStrokeToNative(const Item& item, const Stroke& screenSpaceStroke) {
    Stroke nativeStroke;
    nativeStroke.colorRGBA = screenSpaceStroke.colorRGBA;
    nativeStroke.points.reserve(screenSpaceStroke.points.size());
    float scale = 1.0f;
    for (const StrokePoint& p : screenSpaceStroke.points) {
        const NativePoint native = ScreenToNative(item, p.x, p.y);
        nativeStroke.points.push_back(StrokePoint{native.x, native.y});
        scale = native.scale;  // constant across points within a single call - rect doesn't change mid-call
    }
    nativeStroke.width = screenSpaceStroke.width * scale;
    return nativeStroke;
}

CanvasManagerSnapshot CanvasManager::ExportSnapshot() const {
    CanvasManagerSnapshot snapshot;
    snapshot.folders = folders_;
    snapshot.canvases = canvases_;
    snapshot.currentFolderId = currentFolderId_;
    snapshot.currentCanvasId = currentCanvasId_;
    return snapshot;
}

bool CanvasManager::IsIdTaken(uint64_t id) const {
    for (const Folder& folder : folders_) {
        if (folder.id == id) {
            return true;
        }
    }
    for (const Canvas& canvas : canvases_) {
        if (canvas.id == id) {
            return true;
        }
        for (const Item& item : canvas.items) {
            if (item.id == id) {
                return true;
            }
        }
    }
    return false;
}

uint64_t CanvasManager::NewId() const {
    // Walks the whole library per draw, which is O(everything) - and is
    // the right trade: ids are minted by a human action (a new canvas, a
    // pasted snippet), never in a loop, and a check that cannot go stale
    // is worth more here than one that has to be kept in sync with every
    // path that adds or removes something.
    return MakeUid([this](uint64_t candidate) { return IsIdTaken(candidate); });
}

void CanvasManager::ImportSnapshot(CanvasManagerSnapshot snapshot) {
    folders_ = std::move(snapshot.folders);
    canvases_ = std::move(snapshot.canvases);
    currentFolderId_ = snapshot.currentFolderId;
    currentCanvasId_ = snapshot.currentCanvasId;
    // A library saved looking at something since deleted opens on something
    // that isn't.
    SettleOffDeleted();
}

const Folder* CanvasManager::FindFolder(FolderId id) const {
    const auto it = std::find_if(folders_.begin(), folders_.end(), [id](const Folder& f) { return f.id == id; });
    return it == folders_.end() ? nullptr : &*it;
}

const Canvas* CanvasManager::FindCanvas(CanvasId id) const {
    const auto it = std::find_if(canvases_.begin(), canvases_.end(), [id](const Canvas& c) { return c.id == id; });
    return it == canvases_.end() ? nullptr : &*it;
}

// ================= Deleted things =================

bool CanvasManager::IsDeleted(const Canvas& canvas) const {
    if (canvas.deletedAt != 0) {
        return true;
    }
    const Folder* folder = FindFolder(canvas.folderId);
    return folder != nullptr && IsDeleted(*folder);
}

bool CanvasManager::IsItemDeleted(ItemId id) const {
    for (const Canvas& canvas : canvases_) {
        for (const Item& item : canvas.items) {
            if (item.id == id) {
                return IsDeleted(canvas, item);
            }
        }
    }
    return false;
}

int64_t* CanvasManager::DeletedStampOf(uint64_t id) {
    for (Folder& folder : folders_) {
        if (folder.id == id) {
            return &folder.deletedAt;
        }
    }
    for (Canvas& canvas : canvases_) {
        if (canvas.id == id) {
            return &canvas.deletedAt;
        }
        for (Item& item : canvas.items) {
            if (item.id == id) {
                return &item.deletedAt;
            }
        }
    }
    return nullptr;
}

bool CanvasManager::MarkDeleted(uint64_t id, int64_t when) {
    int64_t* stamp = DeletedStampOf(id);
    if (stamp == nullptr || *stamp != 0) {
        return false;
    }
    // Never 0, which is what "not deleted" is spelled as - a clock that
    // says the epoch still has to leave a mark.
    *stamp = std::max<int64_t>(when, 1);
    SettleOffDeleted();
    MarkChanged();
    return true;
}

bool CanvasManager::Restore(uint64_t id) {
    bool changed = false;
    const auto clear = [&changed](int64_t& stamp) {
        if (stamp != 0) {
            stamp = 0;
            changed = true;
        }
    };
    const auto folderById = [this](FolderId folderId) -> Folder* {
        const auto it =
            std::find_if(folders_.begin(), folders_.end(), [folderId](const Folder& f) { return f.id == folderId; });
        return it == folders_.end() ? nullptr : &*it;
    };
    // A canvas back on its own: out of a deleted folder, the folder comes
    // back and what went with it is marked in its place, as of when it went.
    const auto restoreCanvas = [&](Canvas& canvas) {
        clear(canvas.deletedAt);
        Folder* folder = folderById(canvas.folderId);
        if (folder == nullptr || folder->deletedAt == 0) {
            return;
        }
        const int64_t when = folder->deletedAt;
        clear(folder->deletedAt);
        for (Canvas& other : canvases_) {
            if (other.folderId == folder->id && other.id != canvas.id && other.deletedAt == 0) {
                other.deletedAt = when;
            }
        }
    };

    if (Folder* folder = folderById(id)) {
        clear(folder->deletedAt);
        for (Canvas& canvas : canvases_) {
            if (canvas.folderId == id) {
                clear(canvas.deletedAt);
            }
        }
    } else {
        for (Canvas& canvas : canvases_) {
            if (canvas.id == id) {
                restoreCanvas(canvas);
                break;
            }
            const auto item = std::find_if(canvas.items.begin(), canvas.items.end(),
                                           [id](const Item& i) { return i.id == id; });
            if (item != canvas.items.end()) {
                clear(item->deletedAt);
                restoreCanvas(canvas);
                break;
            }
        }
    }
    if (changed) {
        MarkChanged();
    }
    return changed;
}

bool CanvasManager::Erase(uint64_t id) {
    if (FindFolder(id) != nullptr) {
        DeleteFolder(id);
    } else if (FindCanvas(id) != nullptr) {
        DeleteCanvas(id);
    } else {
        std::optional<CanvasId> holder;
        for (const Canvas& canvas : canvases_) {
            if (std::any_of(canvas.items.begin(), canvas.items.end(), [id](const Item& i) { return i.id == id; })) {
                holder = canvas.id;
                break;
            }
        }
        if (!holder.has_value()) {
            return false;
        }
        DeleteItemFromCanvas(*holder, id);
    }
    // The fallbacks DeleteFolder and DeleteCanvas make know nothing of
    // what is deleted.
    SettleOffDeleted();
    return true;
}

void CanvasManager::SettleOffDeleted() {
    if (const Canvas* current = CurrentOrNull(); current != nullptr && IsDeleted(*current)) {
        const CanvasId currentId = current->id;
        const FolderId folderId = current->folderId;
        const Folder* folder = FindFolder(folderId);
        CanvasId next = 0;
        if (folder != nullptr && !IsDeleted(*folder)) {
            // The live canvas before it in its folder, or else the first
            // after it - the neighbor DeleteCanvas falls back to - and the
            // folder stays the browsed one even with nothing left in it.
            bool passed = false;
            for (const Canvas& canvas : canvases_) {
                if (canvas.id == currentId) {
                    passed = true;
                    continue;
                }
                if (canvas.folderId != folderId || IsDeleted(canvas)) {
                    continue;
                }
                if (!passed) {
                    next = canvas.id;
                } else {
                    if (next == 0) {
                        next = canvas.id;
                    }
                    break;
                }
            }
            currentFolderId_ = folderId;
        } else {
            for (const Canvas& canvas : canvases_) {
                if (!IsDeleted(canvas)) {
                    next = canvas.id;
                    break;
                }
            }
        }
        currentCanvasId_ = next;
        if (const Canvas* now = CurrentOrNull()) {
            currentFolderId_ = now->folderId;
        }
    }
    if (const Folder* browsed = FindFolder(currentFolderId_); browsed != nullptr && IsDeleted(*browsed)) {
        FolderId next = 0;
        if (const Canvas* current = CurrentOrNull()) {
            next = current->folderId;
        } else {
            for (const Folder& folder : folders_) {
                if (!IsDeleted(folder)) {
                    next = folder.id;
                    break;
                }
            }
        }
        currentFolderId_ = next;
    }
}

bool CanvasManager::HoldsDeleted(const Folder& folder) const {
    return IsDeleted(folder) || std::any_of(canvases_.begin(), canvases_.end(), [&folder](const Canvas& canvas) {
               return canvas.folderId == folder.id && canvas.deletedAt != 0;
           });
}

std::vector<CanvasId> CanvasManager::MarkedCanvasesIn(FolderId folderId) const {
    std::vector<CanvasId> marked;
    for (const Canvas& canvas : canvases_) {
        if (canvas.folderId == folderId && canvas.deletedAt != 0) {
            marked.push_back(canvas.id);
        }
    }
    return marked;
}

size_t CanvasManager::DeletedFolderAndCanvasCount() const {
    const auto marked = [](const auto& thing) { return thing.deletedAt != 0; };
    return static_cast<size_t>(std::count_if(folders_.begin(), folders_.end(), marked) +
                               std::count_if(canvases_.begin(), canvases_.end(), marked));
}

std::vector<ItemId> CanvasManager::MarkedSnippets() const {
    std::vector<ItemId> marked;
    for (const Canvas& canvas : canvases_) {
        for (const Item& item : canvas.items) {
            if (item.deletedAt != 0) {
                marked.push_back(item.id);
            }
        }
    }
    return marked;
}

std::vector<uint64_t> CanvasManager::MarkedBefore(int64_t cutoff) const {
    const auto due = [cutoff](int64_t stamp) { return stamp != 0 && stamp < cutoff; };
    std::vector<uint64_t> ids;
    for (const Folder& folder : folders_) {
        if (due(folder.deletedAt)) {
            ids.push_back(folder.id);
        }
    }
    const auto foldersEnd = ids.end();
    std::vector<uint64_t> canvasIds;
    for (const Canvas& canvas : canvases_) {
        if (due(canvas.deletedAt) && std::find(ids.begin(), foldersEnd, canvas.folderId) == foldersEnd) {
            canvasIds.push_back(canvas.id);
        }
    }
    ids.insert(ids.end(), canvasIds.begin(), canvasIds.end());
    return ids;
}

void CanvasManager::SyncShotTexturesToCanvas(CanvasId canvasId,
                                              const std::function<uint64_t(const Item&)>& loadPicture,
                                              const std::function<void(uint64_t)>& releaseTexture) {
    for (Canvas& canvas : canvases_) {
        for (Item& item : canvas.items) {
            // Nothing stored to load from - so nothing to load, and nothing
            // safe to release into.
            Picture& picture = item.picture;
            if (!picture.stored) {
                continue;
            }
            // Only what is on screen needs a texture: a deleted snippet on
            // the current canvas is as far from being drawn as one on
            // another canvas.
            if (canvas.id == canvasId && !IsDeleted(canvas, item)) {
                if (picture.textureHandle == 0) {
                    picture.textureHandle = loadPicture(item);
                }
            } else if (picture.textureHandle != 0) {
                releaseTexture(picture.textureHandle);
                picture.textureHandle = 0;
            }
        }
    }
}

}  // namespace sz::core
