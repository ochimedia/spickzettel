#include "ui/view/screen_chrome.h"

#include <algorithm>
#include <cstdio>
#include <iterator>

#include "core/diagnostics/timeline.h"
#include "platform/platform_types.h"
#include "ui/ui_scale.h"
#include "ui/view/frame_graph.h"
#include "ui/widgets.h"

#include <imgui.h>

namespace sz::ui {

using namespace ::sz::core;

ScreenChrome::ScreenChrome(Session& session, Settings& settings, ViewHost& host)
    : session_(session), settings_(settings), host_(host) {}

// The input readout, under the debug overlay's own lines - see
// AppConfig::showDebugOverlay. What the input options are doing as they
// run, for a report that only reproduces in front of someone else's game:
// the frame rate, the pointer's gain and step sizes while the mouse is
// read raw, how late the countering corrections land, and what is in front
// and where it sits relative to us. The options themselves are the
// Behavior panel's.
void ScreenChrome::DrawInputReadout(ImDrawList* drawList) const {
    if (!Cfg().showDebugOverlay || drawList == nullptr) {
        return;
    }
    const ProfileableSettings& live = settings_.Live();
    const platform::EditModeInputOptions options = live.InputOptions();
    const platform::InputGrabDiagnostics diag =
        host_.Window() != nullptr ? host_.Window()->GetInputGrabDiagnostics() : platform::InputGrabDiagnostics{};

    char fps[160];
    // Availability, not just the stored value: with raw input grayed out
    // there is no pointer of ours being driven, so its gain and step
    // histogram would be a readout of nothing. Per-report step sizes against
    // per-frame ones: all ones in the first and twos in the second means the
    // pointer arithmetic is fine and it is the once-a-frame drawing that
    // looks coarse - a different problem with a different fix.
    if (options.useRawMouseInput && options.RawMouseInputCanBeUsed(live.dontStealFocus) && host_.Window() != nullptr) {
        std::snprintf(fps, sizeof(fps), "%.0f fps  gain %.2f %s  report %d/%d/%d/%d  frame %d/%d/%d/%d",
                       ImGui::GetIO().Framerate, diag.pointerGain, diag.ballisticsEnabled ? "curve" : "flat",
                       diag.stepCounts[0], diag.stepCounts[1], diag.stepCounts[2], diag.stepCounts[3],
                       diag.frameSteps[0], diag.frameSteps[1], diag.frameSteps[2], diag.frameSteps[3]);
    } else {
        std::snprintf(fps, sizeof(fps), "%.0f fps   %.2f ms", ImGui::GetIO().Framerate,
                       1000.0f / std::max(1.0f, ImGui::GetIO().Framerate));
    }

    // What countering is actually managing, when it is on: how long the
    // game had each movement to itself before the negation arrived. The
    // residual the camera keeps is not in here and cannot be - see
    // InputGrabDiagnostics::correctionLagMsLast.
    char counter[192];
    counter[0] = '\0';
    if (options.counterRawMouseInput && options.CounterRawMouseInputCanBeUsed(live.dontStealFocus) &&
        host_.Window() != nullptr) {
        std::snprintf(counter, sizeof(counter), "counter lag %.2f ms (max %.2f)  n=%d  failed=%d",
                       diag.correctionLagMsLast, diag.correctionLagMsMax, diag.correctionsInjected,
                       diag.correctionsFailed);
    }

    // What is in front and where it sits relative to us - the line that
    // answers "why does nothing reach the overlay". Above us, Windows
    // delivers that application's input to no lower-integrity process at
    // all, and no shortcut of the overlay's arrives either. See
    // platform::ForegroundIntegrity.
    char foreground[224];
    const platform::ForegroundApp& app = settings_.UnderlyingApplication();
    const char* what = !app.executable.empty() ? app.executable.c_str()
                       : !app.title.empty()    ? app.title.c_str()
                                               : "(nothing identifiable)";
    switch (app.integrity) {
        case platform::ForegroundIntegrity::Above:
            std::snprintf(foreground, sizeof(foreground), "over %s  -  above us, none of its input reaches here",
                           what);
            break;
        case platform::ForegroundIntegrity::NotAbove:
            std::snprintf(foreground, sizeof(foreground), "over %s  -  not above us", what);
            break;
        case platform::ForegroundIntegrity::Unknown:
        default:
            std::snprintf(foreground, sizeof(foreground), "over %s  -  integrity unreadable", what);
            break;
    }

    // Under the debug overlay's two lines (see DrawDebugOverlay), in its
    // color: one readout, wherever its lines are drawn from.
    constexpr ImU32 kReadoutColor = IM_COL32(0, 255, 255, 255);
    const float lineH = ImGui::GetTextLineHeight();
    ImVec2 at(Px(16.0f), Px(16.0f) + lineH * 2.0f);
    const auto line = [&](const char* text, ImU32 color) {
        drawList->AddText(at, color, text);
        at.y += lineH;
    };
    line(fps, kReadoutColor);
    if (counter[0] != '\0') {
        line(counter, kReadoutColor);
    }
    // Red when it is the answer: above us, nothing else here can be trusted
    // to mean anything.
    line(foreground,
         app.integrity == platform::ForegroundIntegrity::Above ? IM_COL32(232, 100, 100, 255) : kReadoutColor);
}

void ScreenChrome::DrawFrameGraph(ImDrawList* drawList, float displayW) const {
    if (Cfg().showFrameGraph && drawList != nullptr) {
        ui::DrawFrameGraph(drawList, displayW, Timeline::Instance(), Timeline::Now());
    }
}

// The "your clicks land here, not in the game" frame - see
// AppConfig::showEditModeBorder. Drawn only by Draw, which is
// edit-mode-only; view-only mode passes input straight through and so
// has nothing to warn about, and deliberately draws no border of its own.
//
// Inset by half its own width rather than drawn on the screen edge:
// ImDrawList::AddRect centers thickness on the path it's given, so a rect
// at the actual edge would have half of every side clipped away off-screen
// and the border would render at half the width the user asked for.
void ScreenChrome::DrawEditModeBorder(ImDrawList* drawList, float displayW, float displayH) const {
    if (!Cfg().showEditModeBorder || (Cfg().editModeBorderColorRGBA & 0xFFu) == 0 || Cfg().editModeBorderWidthPx <= 0.0f) {
        return;
    }
    if (Cfg().editModeBorderOnlyWhenEmpty) {
        // "Empty" means nothing at all on this canvas - no items (even a
        // blank one still proves the overlay is up) and no un-armed ink on
        // the live layer either. No canvas at all counts as empty too, and
        // is exactly when the cue is worth the most: the screen is
        // otherwise completely blank.
        const Canvas* canvas = Manager().CurrentOrNull();
        const bool anythingShown =
            canvas != nullptr && (!session_.LiveLayer().Strokes().empty() ||
                                  std::any_of(canvas->items.begin(), canvas->items.end(),
                                              [&](const Item& item) { return !Manager().IsDeleted(*canvas, item); }));
        if (anythingShown) {
            return;
        }
    }
    const float half = Cfg().editModeBorderWidthPx * 0.5f;
    drawList->AddRect(ImVec2(half, half), ImVec2(displayW - half, displayH - half),
                       ToImColor(Cfg().editModeBorderColorRGBA), 0.0f, Cfg().editModeBorderWidthPx);
}

// The things that belong over the canvas rather than in it, each in
// its own layer so their heights can be stated rather than inherited from
// where in the frame they happen to be drawn. Called after everything the
// canvas holds and before the Overview, so the whole group sits between
// them - the Overview is the one panel that covers everything, because it
// is the one you go to when something on screen is in the way.
//
// Bottom to top:
//  - The edit-mode border, which is the "your clicks land here" cue: a
//    frame drawn under the snippets is a frame a fullscreen snippet hides
//    completely, which is exactly when the cue matters.
//  - The frame graph and the input readout, in the border's layer, over
//    the snippets for the same reason: a diagnostic a snippet can hide is
//    one that fails when the screen is full.
//
// Nothing here takes input (see BeginScreenLayer), so none of it changes
// what can be clicked, dragged or drawn on.
void ScreenChrome::Draw(float displayW, float displayH) {
    ImDrawList* chrome = BeginScreenLayer("##sz_chrome_layer", displayW, displayH);
    DrawEditModeBorder(chrome, displayW, displayH);
    DrawFrameGraph(chrome, displayW);
    DrawInputReadout(chrome);
    EndScreenLayer();
}

}  // namespace sz::ui
