#pragma once

// A context menu: a popup of action rows, each with an optional icon on
// the left, a short name, and - right-aligned and dimmed - the keyboard
// shortcut that does the same thing. Right-clicking a snippet opens one;
// the dock's canvases and the Recently deleted list are meant to get their
// own, which is why this is a small class of its own rather than another
// Render... member of OverlayApp.
//
// Deliberately knows nothing about the app. It is handed rows, draws them,
// and hands back the `action` of whichever was chosen; every colour comes
// out of the current ImGui style rather than the overlay's own palette
// (see overlay_app_internal.h's theme namespace, which is where that style
// is set from). So this header depends on nothing but Dear ImGui and the
// icon tables, and a menu themes itself with whatever it is dropped into.

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <imgui.h>

#include "ui/icon_draw.h"

namespace sz::ui {

// One row. Built fresh each frame the menu is open, so everything about a
// row - whether it is there at all, what it says, whether it is available -
// can depend on what the menu was opened over.
struct ContextMenuEntry {
    // What the caller gets back when this row is chosen: its own enum,
    // cast to int. The menu never interprets it.
    int action = 0;
    // The row's ImGui id, e.g. "##fullscreen" - what a test clicks it by,
    // and what ImGui hashes. Separate from `label` for the same reason
    // Labeled() exists: the words come from assets/ui_strings.json and must
    // stay rewordable without renaming the widget. Must be unique within
    // the menu.
    const char* id = nullptr;
    // nullptr for a row with no icon of its own. The column is still
    // reserved when any row in the menu has one, so the labels line up.
    const Icon* icon = nullptr;
    const char* label = nullptr;
    // "Ctrl+D", or empty for an action with no shortcut. A std::string
    // rather than a const char* because this is usually built per frame
    // from the live binding (see FormatKeyComboLabel) - the label, which
    // comes straight out of the string catalogue, is not.
    std::string shortcut;
    // A row that is visible but cannot be chosen right now - greyed, and
    // it takes no click. Shown rather than dropped so the menu keeps the
    // same shape whatever it is opened over, which is what lets a hand
    // learn where a row is.
    bool enabled = true;
    // Draws a divider above this row. Ignored on the first row, where
    // there is nothing to divide from.
    bool separatorAbove = false;
};

class ContextMenu {
public:
    // Fills in the rows for this frame. Called only while the menu is
    // actually up, so a caller pays nothing for a menu nobody opened - and
    // leaving it empty (the snippet it was opened over has been deleted,
    // say) closes the menu rather than drawing an empty panel.
    using Builder = std::function<void(std::vector<ContextMenuEntry>&)>;

    // `popupId` is the ImGui popup id and must outlive the menu - a string
    // literal, in practice.
    explicit ContextMenu(const char* popupId) : popupId_(popupId) {}

    // Opens the menu with its corner at `screenPos`, on the next frame.
    //
    // Deferred rather than immediate because the press that asks for it
    // usually arrives on the raw mouse callback, which runs between frames
    // where ImGui::OpenPopup has no current window to scope its id against
    // - a verified null dereference, not a hypothetical one. See
    // OverlayApp::itemPropertiesPopoverRequested_ for the long version.
    void RequestOpenAt(ImVec2 screenPos);

    // Whether the menu is on screen right now - which is not the same as
    // having been asked for: the request is consumed by the next Render.
    bool IsOpen() const { return open_; }

    // Draws the menu if it is open. Returns the `action` of the row chosen
    // this frame, if any; choosing a row also closes the menu.
    std::optional<int> Render(const Builder& build);

private:
    const char* popupId_;
    bool requested_ = false;
    bool open_ = false;
    ImVec2 anchor_{0.0f, 0.0f};
};

}  // namespace sz::ui
