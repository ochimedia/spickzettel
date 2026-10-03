#pragma once

// The canvas bar - docs/VIEW_LAYER.md, section 7: the panel that hides
// against the bottom edge of the screen and slides out when it is wanted,
// with the canvases of the folder the current canvas is in, as
// thumbnails, the current one outlined, and a button each for a new canvas
// and the Overview. What it keeps of its own is how far out it is, where it
// was drawn, how far its tiles are scrolled, the canvas it last saw, and
// the requests to come out for a moment and to bring the current tile into
// view.

#include <algorithm>
#include <optional>
#include <vector>

#include "core/canvas/canvas.h"
#include "core/canvas/item_geometry.h"
#include "core/session/session.h"
#include "core/session/settings.h"
#include "ui/editor.h"
#include "ui/view/view_host.h"

namespace sz::ui {

// How far out a panel that hides against an edge of the screen is: 0 all
// the way in, 1 all the way out. It slides out while it is wanted or until
// `holdUntil` (a flash), and back in once it has gone unwanted for
// kEdgeRevealLingerSeconds - long enough that crossing a gap between the
// edge and the panel doesn't send it away. See CanvasBar::Update.
inline constexpr float kEdgeRevealSlideSeconds = 0.14f;
inline constexpr double kEdgeRevealLingerSeconds = 0.45;
struct EdgeReveal {
    float amount = 0.0f;
    double holdUntil = 0.0;
    double lastWanted = -1.0e9;
    void Update(bool wanted, double now, float deltaSeconds);
    void Flash(double now, double seconds) { holdUntil = std::max(holdUntil, now + seconds); }
};

class CanvasBar {
public:
    CanvasBar(core::Session& session, core::Settings& settings, Editor& editor, ViewHost& host);

    // Where the bar is this frame and how far out, from whether it is
    // wanted - the pointer at the bottom edge or on the bar, or its tile
    // menu up (`menuUp`) - and from what has happened (the overlay coming
    // up, the canvas changing). Once per frame, before anything is drawn,
    // so that the dock's chips, which have to clear it, and the bar's own
    // window read the same answer.
    void Update(float displayW, float displayH, bool menuUp);
    // The bar, where Update put it.
    void Draw(float displayW, float displayH);

    // Out for a moment on the next frame, so it is seen where it is - the
    // overlay has just come up.
    void Flash() { edgePanelsFlashPending_ = true; }
    // How far out it is, 0 to 1.
    float Reveal() const { return canvasBarReveal_.amount; }
    // The top of whatever is out on the bottom edge this frame, or the
    // display's height - what the dock's chips have to stay above.
    float BottomPanelsTop() const { return bottomPanelsTop_; }

private:
    const core::CanvasManager& Manager() const { return session_.Manager(); }
    const core::AppConfig& Cfg() const { return settings_.Stored(); }
    // Whether a panel covering the canvas is up - under which the bar stays
    // in.
    bool PanelOpen() const;
    // The canvases of the folder the current canvas is in, in the folder's
    // order.
    std::vector<core::CanvasId> CanvasBarCanvases() const;

    core::Session& session_;
    core::Settings& settings_;
    Editor& editor_;
    ViewHost& host_;

    // How far out it is, and where it was drawn this frame (none while it
    // is all the way in).
    EdgeReveal canvasBarReveal_;
    std::optional<core::Rect> canvasBarRect_;
    // The top of whatever is out on the bottom edge this frame, or the
    // display's height.
    float bottomPanelsTop_ = 0.0f;
    // The tiles, scrolled this far; the canvas it last saw, to notice a
    // change; and whether to bring the current tile into view.
    float canvasBarScroll_ = 0.0f;
    std::optional<core::CanvasId> canvasBarLastCanvas_;
    bool canvasBarScrollToCurrent_ = true;
    // Set when the overlay comes up, for the bar to come out for a moment on
    // the first frame after.
    bool edgePanelsFlashPending_ = false;
    // The tile the right button went down on, until it comes up: its menu
    // opens on the release, over the same tile.
    std::optional<core::CanvasId> rightPressedTile_;
};

}  // namespace sz::ui
