#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/canvas/item.h"
#include "core/drawing/canvas_state.h"

namespace sz::core {

using CanvasId = uint64_t;
using FolderId = uint64_t;

// A named group of canvases - one per game, or per set of levels within a
// game. Folders don't nest: this is a flat list. No slug is stored: the
// directory a folder lives in is named from `name` and `id` afresh on
// every save (see util/slug.h).
struct Folder {
    FolderId id = 0;
    std::string name;
    // When it was made and, while it is deleted, when it was deleted - both
    // seconds since the epoch, 0 for "never". The stamp is the whole of
    // being deleted: a deleted thing stays where it is, marked, and
    // restoring it clears the mark (see CanvasManager::MarkDeleted).
    int64_t createdAt = 0;
    int64_t deletedAt = 0;
};

// An independent collection of items. `items` order is paint order:
// index 0 is the bottom of the stack (see CanvasManager::MoveItemLayer).
struct Canvas {
    CanvasId id = 0;
    std::string name;
    // Which folder this canvas is in. In memory only: on disk the directory
    // it sits in says so - see LibraryStore.
    FolderId folderId = 0;
    // See Folder::createdAt/deletedAt.
    int64_t createdAt = 0;
    int64_t deletedAt = 0;
    std::vector<Item> items;
    // Scratch space, not content: a stroke accumulates here in screen space
    // while it is being drawn and is moved into the snippet in drawing mode
    // the moment it ends (see Session::CommitLiveStroke). Empty at every
    // point that isn't mid-gesture, and never persisted. Per canvas rather
    // than shared, so an in-progress stroke can't leak across a canvas
    // switch.
    CanvasState liveLayer;
};

}  // namespace sz::core
