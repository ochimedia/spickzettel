#pragma once

// The Behavior panel - docs/VIEW_LAYER.md, section 7: edit mode's input
// options, the switches of Settings > Behavior that decide how the overlay
// sits over a game, a number key each. On its own global hotkey, so it can
// be reached when the mouse cannot be trusted: a combination being tried
// that sends clicks to the overlay and the game at once is exactly when
// Settings is hard to use.
//
// A window in a corner, over nothing: the game, the snippets and the
// canvas stay in view and in reach while it is up, since seeing them
// behave is how a combination is judged. So it is no panel on the
// machine's Panel level, which covers the canvas; it keeps whether it is
// up itself, and the Canvas level turns its digits into a command (see
// EditorViews::BehaviorPanelRow). Every other key goes where it would
// without it, the game's included (see IOverlayWindow::SetPanelDigits).

#include <optional>

#include "core/session/settings.h"
#include "ui/editor.h"
#include "ui/view/view_host.h"

namespace sz::ui {

class BehaviorPanel {
public:
    BehaviorPanel(core::Settings& settings, ViewHost& host);

    bool IsOpen() const { return open_; }
    void Open() { open_ = true; }
    // Up if it was not, and put away if it was - its own key.
    void Toggle() { open_ = !open_; }
    // The window, while it is up, in edit mode.
    void Draw();
    // The row a bare digit names, while it is up in edit mode - a held
    // one's repeats included. None for any other key, which goes on to the
    // canvas.
    std::optional<int> RowFor(const Event& event, bool editMode) const;
    // The row switched, into the profile that runs - the panel is about the
    // configuration in front of you - and the overlay restarted for a row
    // read only on the way up. A grayed row is not.
    void Switch(int row);
    // Once a frame, after the draw: the window told how many digits the
    // panel takes - none but in edit mode - and the overlay restarted once
    // a row that needs it has had its key or button let go of.
    void Update(bool editMode);

private:
    // A row's value as it runs - the resolved one, a profile's where one
    // matches - and whether it can do anything with the rows above as they
    // are.
    bool Value(int row) const;
    bool Available(int row) const;
    void DrawRows();

    core::Settings& settings_;
    ViewHost& host_;
    bool open_ = false;
    // What the window was last told (see Update): telling it again can
    // install or remove a keyboard hook.
    int appliedDigits_ = 0;
    // A switch that needs edit mode entered again, waiting for the key or
    // button that made it to come up: a restart takes the window down with
    // the key still held, its up is lost, and the next press of it is no
    // press at all.
    bool pendingRestart_ = false;
};

}  // namespace sz::ui
