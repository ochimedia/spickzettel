#pragma once

// The places on screen something may point at - docs/TUTORIAL.md, section
// 7.2 - and a board of where each is this frame. An owner that draws an
// anchored widget marks it as it draws it, through ViewHost::Mark; the
// board is cleared at the start of every frame, so an anchor not drawn in
// this one is not on screen. The owners know only that there is a board:
// what reads it - the tutorial's spotlight - is none of theirs.

#include <algorithm>
#include <optional>
#include <vector>

#include <imgui.h>

#include "core/canvas/item.h"

namespace sz::ui {

enum class AnchorId {
    // The selection bar's close button and its pin, and the drawing bar's
    // pen.
    SelectionBarClose,
    SelectionBarPin,
    DrawingBarPen,
    // A minimized snippet's chip in the dock, by its snippet.
    DockChip,
};

// An anchor, and the snippet it is for when there is one on screen per
// snippet.
struct Anchor {
    AnchorId id = AnchorId::SelectionBarClose;
    core::ItemId item = 0;

    bool operator==(const Anchor&) const = default;
};

// Where an anchor is: a rectangle in screen pixels.
struct AnchorRect {
    ImVec2 min;
    ImVec2 max;
};

class AnchorBoard {
public:
    // At the start of a frame, before anything is drawn.
    void Clear() { marks_.clear(); }
    // Drawn this frame at `rect` - the last mark of an anchor wins.
    void Mark(Anchor anchor, AnchorRect rect) {
        const auto it = std::find_if(marks_.begin(), marks_.end(), [&](const Entry& m) { return m.anchor == anchor; });
        if (it != marks_.end()) {
            it->rect = rect;
        } else {
            marks_.push_back(Entry{anchor, rect});
        }
    }
    // Where `anchor` was drawn this frame, if it was.
    std::optional<AnchorRect> Find(Anchor anchor) const {
        const auto it = std::find_if(marks_.begin(), marks_.end(), [&](const Entry& m) { return m.anchor == anchor; });
        return it != marks_.end() ? std::optional<AnchorRect>(it->rect) : std::nullopt;
    }

private:
    struct Entry {
        Anchor anchor;
        AnchorRect rect;
    };
    // A handful a frame at most: a list, looked through.
    std::vector<Entry> marks_;
};

}  // namespace sz::ui
