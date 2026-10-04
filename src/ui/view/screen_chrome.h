#pragma once

// The screen chrome - docs/VIEW_LAYER.md, section 7: what sits over the
// canvas rather than in it, each in a layer of its own so that its height
// is stated rather than inherited from where in the frame it is drawn -
// the input options HUD, the edit-mode border and the frame graph. None
// of it takes input. What it keeps of its own is the HUD's applied digits,
// its restart waiting for a key to come up and the record of its last key.

#include <imgui.h>

#include "core/session/session.h"
#include "core/session/settings.h"
#include "ui/interaction/event.h"
#include "ui/view/view_host.h"

namespace sz::ui {

class ScreenChrome {
public:
    ScreenChrome(core::Session& session, core::Settings& settings, ViewHost& host);

    // The HUD's layer, then the border's, over
    // everything the canvas holds and under the Overview - see the
    // definition for the order and why each is where it is.
    void Draw(float displayW, float displayH);
    // A debugging aid, off by default - see AppConfig::showFrameGraph. Into
    // `drawList`: the chrome's layer in edit mode, the view-only layer
    // otherwise.
    void DrawFrameGraph(ImDrawList* drawList, float displayW) const;
    // Once a frame, after the draw: the window told how many number keys the
    // HUD takes - none but in edit mode - and the overlay restarted once a
    // toggle that needs it has had its key let go of.
    void Update(bool editMode);
    // A number key the HUD advertises, as it goes down: toggles the option,
    // persists it, and asks for edit mode to be re-entered for an option
    // that only applies on entry. False for a key that is not the HUD's.
    bool HandleKey(const Event& event, bool editMode);

private:
    const core::AppConfig& Cfg() const { return settings_.Stored(); }
    const core::CanvasManager& Manager() const { return session_.Manager(); }

    // See AppConfig::showEditModeBorder.
    void DrawEditModeBorder(ImDrawList* drawList, float displayW, float displayH) const;
    // A debugging aid: the state of every capture/input option, top-left,
    // with a number key per row that toggles it. These options interact in
    // ways that are only discoverable by trying combinations, and reaching
    // the Overview to change one is several clicks away from the situation
    // being tested. See the definition for what each row is.
    void DrawInputOptionsHud(ImDrawList* drawList) const;
    // What a HUD row currently reads - the resolved value: the HUD reports
    // what is running - and whether it can do anything.
    bool InputOptionValue(int index) const;
    bool InputOptionAvailable(int index) const;

    core::Session& session_;
    core::Settings& settings_;
    ViewHost& host_;

    // The last count handed to IOverlayWindow::SetInputOptionsHudDigits, so
    // that only an actual change reaches the platform - it can install or
    // remove a keyboard hook, which is not something to ask for every frame.
    int appliedInputOptionsHudDigits_ = 0;
    // A HUD toggle that needs edit mode re-entered waits here until the key is
    // released - see Update for why holding the restart is the difference
    // between every press counting and one in three vanishing.
    bool pendingOverlayRestart_ = false;

    // What the HUD's last number key actually did, shown in the HUD itself.
    //
    // For a report that only reproduces on someone else's machine: "pressing
    // 1 sometimes switches back" has two very different causes - the key
    // acting twice, or acting once and something else undoing it - and from
    // the outside they look identical. The toggle count tells them apart at
    // a glance, and the rest says where the value was written and what it
    // resolved to afterwards.
    int hudToggleCount_ = 0;
    int hudLastToggledRow_ = 0;      // 1-based, 0 for "nothing yet"
    bool hudLastToggledTo_ = false;  // what that press asked for
    bool hudLastWentToProfile_ = false;
};

}  // namespace sz::ui
