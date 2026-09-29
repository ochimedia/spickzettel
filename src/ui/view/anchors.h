#pragma once

// The places on screen something may point at - docs/TUTORIAL.md, section
// 7.2 - and a board of where each is this frame. An owner that draws an
// anchored widget marks it as it draws it, through ViewHost::Mark; the
// board is cleared at the start of every frame, so an anchor not drawn in
// this one is not on screen. The owners know only that there is a board:
// what reads it - the tutorial's spotlight - is none of theirs.

#include <algorithm>
#include <cstdint>
#include <optional>
#include <vector>

#include <imgui.h>

namespace sz::ui {

enum class AnchorId {
    // The selection bar's close button and its pin, and the drawing bar's
    // buttons.
    SelectionBarClose,
    SelectionBarPin,
    DrawingBarPen,
    DrawingBarEraser,
    DrawingBarText,
    DrawingBarColor,
    // A minimized snippet's chip in the dock, by its snippet.
    DockChip,
    // The canvas bar's two buttons: a new canvas, and the Overview.
    CanvasBarNew,
    CanvasBarOverview,
    // The Overview's Settings tab.
    OverviewSettingsTab,
    // The Overview's New folder and Show deleted; a folder's row, by its
    // folder; a canvas's tile and the trash button under it, by its
    // canvas; and the Restore of something deleted, by its folder or
    // canvas.
    OverviewNewFolder,
    OverviewShowDeleted,
    OverviewFolderRow,
    OverviewCanvasTile,
    OverviewCanvasDelete,
    OverviewRestore,
    // The Settings panel's section buttons, by section; Make a profile for
    // this and New profile; a profile's trash button, by its place in the
    // list; Showing, and each entry of its list - 0 the defaults, then each
    // profile by its place plus one; the Don't steal focus row; and the
    // revert arrow of each Behavior row the target states, by the row's
    // place in the section.
    SettingsSection,
    SettingsMakeProfile,
    SettingsNewProfile,
    SettingsDeleteProfile,
    SettingsShowing,
    SettingsShowingEntry,
    SettingsDontStealFocus,
    SettingsRevert,
};

// An anchor, and what it is for when there is one on screen per snippet,
// folder or canvas: its id.
struct Anchor {
    AnchorId id = AnchorId::SelectionBarClose;
    uint64_t of = 0;

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
