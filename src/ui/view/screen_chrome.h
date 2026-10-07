#pragma once

// The screen chrome - docs/VIEW_LAYER.md, section 7: what sits over the
// canvas rather than in it, each in a layer of its own so that its height
// is stated rather than inherited from where in the frame it is drawn -
// the edit-mode border, the frame graph and the input readout. None of it
// takes input, and it keeps nothing of its own.

#include <imgui.h>

#include "core/session/session.h"
#include "core/session/settings.h"
#include "ui/view/view_host.h"

namespace sz::ui {

class ScreenChrome {
public:
    ScreenChrome(core::Session& session, core::Settings& settings, ViewHost& host);

    // The chrome's layer, over everything the canvas holds and under the
    // Overview - see the definition for the order and why each is where it
    // is.
    void Draw(float displayW, float displayH);
    // A debugging aid, off by default - see AppConfig::showFrameGraph. Into
    // `drawList`: the chrome's layer in edit mode, the view-only layer
    // otherwise.
    void DrawFrameGraph(ImDrawList* drawList, float displayW) const;

private:
    const core::AppConfig& Cfg() const { return settings_.Stored(); }
    const core::CanvasManager& Manager() const { return session_.Manager(); }

    // See AppConfig::showEditModeBorder.
    void DrawEditModeBorder(ImDrawList* drawList, float displayW, float displayH) const;
    // A debugging aid, with the debug overlay - see DrawInputReadout.
    void DrawInputReadout(ImDrawList* drawList) const;

    core::Session& session_;
    core::Settings& settings_;
    ViewHost& host_;
};

}  // namespace sz::ui
