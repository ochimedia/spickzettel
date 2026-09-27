#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/canvas/item.h"

namespace sz::core {

using CanvasId = uint64_t;
using FolderId = uint64_t;

// A named group of canvases - one per game, or per set of levels within a
// game. Folders don't nest: this is a flat list.
struct Folder {
    FolderId id = 0;
    std::string name;
    // When it was made and, while it is deleted, when it was deleted - both
    // seconds since the epoch. 0 for not deleted, and for made before this
    // was stamped (see docs/ARCHITECTURE.md, "Schema"). The deletion stamp
    // is the whole of being deleted: a deleted thing stays where it is,
    // marked, and restoring it clears the mark (see
    // CanvasManager::MarkDeleted).
    int64_t createdAt = 0;
    int64_t deletedAt = 0;

    bool operator==(const Folder&) const = default;
};

// An independent collection of items. `items` order is paint order:
// index 0 is the bottom of the stack (see CanvasManager::MoveItemLayer).
struct Canvas {
    CanvasId id = 0;
    std::string name;
    // Which folder this canvas is in.
    FolderId folderId = 0;
    // See Folder::createdAt/deletedAt.
    int64_t createdAt = 0;
    int64_t deletedAt = 0;
    std::vector<Item> items;
};

}  // namespace sz::core
